// src/dumper/dumper.cpp — the ioctl-codex IL2CPP / Unity dumper.
//
// Builds as libioctl_codex_dumper.so and is loaded as an ordinary custom lib.
// It exports the stable entry point ioctl_codex_dumper_run(), which the overlay
// invokes from the "Unity" tab.
//
// DESIGN CHOICE — runtime walking, not static metadata parsing:
//   Il2CppDumper reads the on-disk global-metadata.dat and needs the exact
//   metadata version (24/27/29/31) plus a matching struct layout. In-process we
//   have something strictly better: the engine has already parsed everything
//   and exposes it through the exported il2cpp_* C API. So we enumerate
//   assemblies -> images -> classes -> fields/methods through that API and only
//   fall back to reading raw pointers where no accessor exists (method RVA).
//   This is version-agnostic and immune to metadata encryption/obfuscation of
//   the on-disk file.
//
// OUTPUT — exactly three files under <dump_root>/<package>/<Version>/:
//   dump.cs   Il2CppDumper-compatible class/method listing with RVAs
//   bypass.h  hook macros + offset table + bypass stubs
//   il2cpp.h  C struct/type definitions with field offsets
//
// The sandboxed target cannot write /sdcard, so everything is written to
// <stage_root>/<pkg>/<ver>/ and then handed to ctx->publish_dir(), which asks
// the module's root companion to move it into place.

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ioctl_codex.h"

#define LOG_TAG "ioctl-codex-dumper"

namespace {

const IoctlCodexCtx *g_ctx = nullptr;

void logmsg(int level, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_ctx && g_ctx->log) g_ctx->log(level, "dumper", buf);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", buf);
}

// --------------------------------------------------------------------------
// IL2CPP runtime API resolved dynamically.
// --------------------------------------------------------------------------
struct Il2CppApi {
    void *handle = nullptr;
    uintptr_t base = 0;       // load base of libil2cpp.so (VA of file offset 0)
    uintptr_t base_end = 0;

    // Opaque runtime types (we only pass them back into the API).
    struct Il2CppClass;      // forward
    struct Il2CppImage;
    struct Il2CppAssembly;
    struct Il2CppDomain;
    struct Il2CppType;
    struct FieldInfo;
    struct MethodInfo;

    // Accessors
    Il2CppDomain *(*domain_get)();
    const Il2CppAssembly **(*domain_get_assemblies)(Il2CppDomain *, size_t *);
    Il2CppImage *(*assembly_get_image)(const Il2CppAssembly *);
    const char *(*image_get_name)(const Il2CppImage *);
    const char *(*image_get_filename)(const Il2CppImage *);
    size_t (*image_get_class_count)(const Il2CppImage *);
    Il2CppClass *(*image_get_class)(const Il2CppImage *, size_t);
    const char *(*class_get_name)(Il2CppClass *);
    const char *(*class_get_namespace)(Il2CppClass *);
    Il2CppClass *(*class_get_parent)(Il2CppClass *);
    Il2CppType *(*class_get_type)(Il2CppClass *);
    uint32_t (*class_get_flags)(Il2CppClass *);
    bool (*class_is_enum)(Il2CppClass *);
    bool (*class_is_valuetype)(Il2CppClass *);
    bool (*class_is_interface)(Il2CppClass *);
    bool (*class_is_abstract)(Il2CppClass *);
    Il2CppClass *(*class_get_declaring_type)(Il2CppClass *);
    const Il2CppClass **(*class_get_interfaces)(Il2CppClass *, void **);
    FieldInfo *(*class_get_fields)(Il2CppClass *, void **);
    const MethodInfo *(*class_get_methods)(Il2CppClass *, void **);
    const char *(*field_get_name)(FieldInfo *);
    Il2CppType *(*field_get_type)(FieldInfo *);
    size_t (*field_get_offset)(FieldInfo *);
    uint32_t (*field_get_flags)(FieldInfo *);
    const char *(*method_get_name)(const MethodInfo *);
    const Il2CppType *(*method_get_return_type)(const MethodInfo *);
    uint32_t (*method_get_param_count)(const MethodInfo *);
    const Il2CppType *(*method_get_param)(const MethodInfo *, uint32_t);
    const char *(*method_get_param_name)(const MethodInfo *, uint32_t);
    uint32_t (*method_get_flags)(const MethodInfo *, uint32_t *);
    Il2CppClass *(*method_get_declaring_type)(const MethodInfo *);
    const char *(*type_get_name)(const Il2CppType *);
    uint32_t (*type_get_attrs)(const Il2CppType *);
    void (*free_)(void *);
    // Optional
    void *(*method_get_pointer)(const MethodInfo *);

    template <typename T>
    bool sym(T &out, const char *name) {
        out = reinterpret_cast<T>(dlsym(handle, name));
        return out != nullptr;
    }

    bool load();
};

// Find libil2cpp.so: first via RTLD_NOLOAD, then by scanning /proc/self/maps.
void *find_il2cpp() {
    void *h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (h) return h;

    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return nullptr;
    char line[4096];
    std::string path;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "libil2cpp.so")) continue;
        // last field is the pathname
        char *p = strchr(line, '/');
        if (p) { path = p; while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back(); break; }
    }
    fclose(f);
    if (path.empty()) return nullptr;
    return dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
}

// Compute the load base (VA at file offset 0) of the module that contains
// `h`'s path, so RVAs are file-relative like Il2CppDumper's.
void compute_base(Il2CppApi &api) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "libil2cpp.so")) continue;
        unsigned long start = 0, end = 0, off = 0;
        // format: start-end perms offset dev inode path
        if (sscanf(line, "%lx-%lx %*s %lx", &start, &end, &off) != 3) continue;
        if (off == 0) {
            api.base = start;         // base = start - off
            api.base_end = end;
            // Extend base_end to the highest mapping end we see.
        }
        if (start > api.base_end) api.base_end = end;
    }
    fclose(f);
}

bool Il2CppApi::load() {
    handle = find_il2cpp();
    if (!handle) { logmsg(IOCTL_CODEX_LOG_ERROR, "libil2cpp.so not loaded (not a Unity/IL2CPP target)"); return false; }
    compute_base(*this);

    bool ok = true;
    ok &= sym(domain_get, "il2cpp_domain_get");
    ok &= sym(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    ok &= sym(assembly_get_image, "il2cpp_assembly_get_image");
    ok &= sym(image_get_name, "il2cpp_image_get_name");
    sym(image_get_filename, "il2cpp_image_get_filename");   // optional
    ok &= sym(image_get_class_count, "il2cpp_image_get_class_count");
    ok &= sym(image_get_class, "il2cpp_image_get_class");
    ok &= sym(class_get_name, "il2cpp_class_get_name");
    ok &= sym(class_get_namespace, "il2cpp_class_get_namespace");
    ok &= sym(class_get_parent, "il2cpp_class_get_parent");
    ok &= sym(class_get_type, "il2cpp_class_get_type");
    ok &= sym(class_get_flags, "il2cpp_class_get_flags");
    ok &= sym(class_is_enum, "il2cpp_class_is_enum");
    ok &= sym(class_is_valuetype, "il2cpp_class_is_valuetype");
    ok &= sym(class_is_interface, "il2cpp_class_is_interface");
    ok &= sym(class_is_abstract, "il2cpp_class_is_abstract");
    sym(class_get_declaring_type, "il2cpp_class_get_declaring_type");
    sym(class_get_interfaces, "il2cpp_class_get_interfaces");
    ok &= sym(class_get_fields, "il2cpp_class_get_fields");
    ok &= sym(class_get_methods, "il2cpp_class_get_methods");
    ok &= sym(field_get_name, "il2cpp_field_get_name");
    ok &= sym(field_get_type, "il2cpp_field_get_type");
    ok &= sym(field_get_offset, "il2cpp_field_get_offset");
    ok &= sym(field_get_flags, "il2cpp_field_get_flags");
    ok &= sym(method_get_name, "il2cpp_method_get_name");
    ok &= sym(method_get_return_type, "il2cpp_method_get_return_type");
    ok &= sym(method_get_param_count, "il2cpp_method_get_param_count");
    ok &= sym(method_get_param, "il2cpp_method_get_param");
    sym(method_get_param_name, "il2cpp_method_get_param_name");
    ok &= sym(method_get_flags, "il2cpp_method_get_flags");
    ok &= sym(method_get_declaring_type, "il2cpp_method_get_declaring_type");
    ok &= sym(type_get_name, "il2cpp_type_get_name");
    sym(type_get_attrs, "il2cpp_type_get_attrs");
    sym(free_, "il2cpp_free");
    sym(method_get_pointer, "il2cpp_method_get_pointer");

    if (!ok) {
        logmsg(IOCTL_CODEX_LOG_ERROR, "required il2cpp symbols missing; cannot dump");
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// Type / name helpers
// --------------------------------------------------------------------------
std::string type_name(const Il2CppApi &api, const Il2CppApi::Il2CppType *t) {
    if (!t) return "void";
    if (api.type_get_name) {
        const char *n = api.type_get_name(t);
        if (n) {
            std::string s = n;
            if (api.free_) api.free_((void *)n);
            return s;
        }
    }
    return "object";
}

// Map IL2CPP method flags (MethodAttributes) to a C# modifier string.
std::string method_mods(uint32_t flags) {
    std::string s;
    uint32_t access = flags & 0x0007; // MemberAccessMask
    switch (access) {
        case 0x0001: s += "private "; break;
        case 0x0002: s += "protected "; break;
        case 0x0003: s += "internal "; break;
        case 0x0004: s += "protected internal "; break;
        case 0x0006: s += "public "; break;
        default: break;
    }
    if (flags & 0x0010) s += "static ";
    if (flags & 0x0020) s += "final ";
    if (flags & 0x0040) s += "virtual ";
    if (flags & 0x0400) s += "abstract ";
    return s;
}

// Map IL2CPP field flags to a C# modifier string.
std::string field_mods(uint32_t flags) {
    std::string s;
    uint32_t access = flags & 0x0007;
    switch (access) {
        case 0x0001: s += "private "; break;
        case 0x0002: s += "protected "; break;
        case 0x0003: s += "internal "; break;
        case 0x0004: s += "protected internal "; break;
        case 0x0006: s += "public "; break;
        default: break;
    }
    if (flags & 0x0010) s += "static ";
    if (flags & 0x0020) s += "readonly ";
    if (flags & 0x0040) s += "const ";
    return s;
}

uint32_t class_token(const Il2CppApi::Il2CppClass *) { return 0; }

// --------------------------------------------------------------------------
// Output sink (buffered file writer)
// --------------------------------------------------------------------------
struct OutFile {
    FILE *f = nullptr;
    std::string path;
    bool open(const std::string &p) { path = p; f = fopen(p.c_str(), "wb"); return f != nullptr; }
    void put(const char *fmt, ...) {
        if (!f) return;
        va_list ap; va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
    }
    void puts(const std::string &s) { if (f) fwrite(s.data(), 1, s.size(), f); }
    void close() { if (f) { fclose(f); f = nullptr; } }
    ~OutFile() { close(); }
};

std::string join_ns(const char *ns, const char *name) {
    std::string n = name ? name : "";
    if (ns && *ns) return std::string(ns) + "." + n;
    return n;
}

// --------------------------------------------------------------------------
// dump.cs
// --------------------------------------------------------------------------
void write_dump_cs(const Il2CppApi &api, OutFile &out) {
    out.put("// ioctl-codex IL2CPP dump — runtime-generated, Il2CppDumper-compatible\n");
    out.put("// target : %s\n", g_ctx && g_ctx->package_name ? g_ctx->package_name : "?");
    out.put("// base   : 0x%" PRIxPTR "\n", api.base);
    out.put("// fields : Name, RVA (relative to libil2cpp.so base), Offset\n\n");

    auto *domain = api.domain_get();
    if (!domain) { out.put("// no IL2CPP domain (engine not initialised yet)\n"); return; }
    size_t asm_count = 0;
    const Il2CppApi::Il2CppAssembly **asms = api.domain_get_assemblies(domain, &asm_count);
    if (!asms) { out.put("// no assemblies\n"); return; }

    for (size_t a = 0; a < asm_count; ++a) {
        auto *image = api.assembly_get_image(asms[a]);
        if (!image) continue;
        const char *img_name = api.image_get_name ? api.image_get_name(image) : "?";
        size_t cls_count = api.image_get_class_count(image);
        out.put("// Image %zu: %s (%zu types)\n", a, img_name ? img_name : "?", cls_count);

        for (size_t c = 0; c < cls_count; ++c) {
            auto *klass = api.image_get_class(image, c);
            if (!klass) continue;

            const char *name = api.class_get_name(klass);
            const char *ns = api.class_get_namespace(klass);
            uint32_t flags = api.class_get_flags(klass);
            bool is_enum = api.class_is_enum(klass);
            bool is_iface = api.class_is_interface(klass);
            bool is_abstract = api.class_is_abstract(klass);

            out.put("// Namespace: %s\n", ns && *ns ? ns : "");
            std::string kind = is_enum ? "enum" : (is_iface ? "interface" : (api.class_is_valuetype(klass) ? "struct" : "class"));
            uint32_t access = flags & 0x0007;
            std::string mods;
            if (access == 0x0001) mods = "internal ";
            else if (access == 0x0006) mods = "public ";
            if (!is_iface && (flags & 0x0080)) mods += "abstract ";  // abstract class
            out.put("%s%s %s", mods.c_str(), kind.c_str(), name ? name : "?");

            auto *parent = api.class_get_parent(klass);
            if (parent && !is_enum) {
                const char *pn = api.class_get_name(parent);
                const char *pns = api.class_get_namespace(parent);
                std::string pf = join_ns(pns, pn);
                if (pf != "System.Object" && pf != "System.ValueType" && pf != "System.Enum")
                    out.put(" : %s", pf.c_str());
            }
            // Interfaces
            if (api.class_get_interfaces) {
                void *iter = nullptr;
                const Il2CppApi::Il2CppClass **iface;
                bool first = true;
                while ((iface = api.class_get_interfaces(klass, &iter)) && *iface) {
                    // class_get_name/namespace take a non-const Il2CppClass*; the
                    // interface list is exposed as const, so drop const here.
                    auto *ic = const_cast<Il2CppApi::Il2CppClass *>(*iface);
                    const char *in = api.class_get_name(ic);
                    const char *ins = api.class_get_namespace(ic);
                    out.put("%s %s", first && !(parent && !is_enum) ? " :" : (first ? " :" : ","), join_ns(ins, in).c_str());
                    first = false;
                }
            }
            out.put("\n{\n");

            // Fields
            if (api.class_get_fields) {
                void *iter = nullptr;
                Il2CppApi::FieldInfo *fld;
                while ((fld = api.class_get_fields(klass, &iter))) {
                    const char *fn = api.field_get_name(fld);
                    auto *ft = api.field_get_type(fld);
                    size_t off = api.field_get_offset(fld);
                    uint32_t ff = api.field_get_flags(fld);
                    std::string tn = type_name(api, ft);
                    out.put("    // RVA: 0x0 Offset: 0x%zX\n", off);
                    out.put("    %s%s %s; // 0x%zX\n", field_mods(ff).c_str(), tn.c_str(), fn ? fn : "?", off);
                }
            }

            // Methods
            if (api.class_get_methods) {
                void *iter = nullptr;
                const Il2CppApi::MethodInfo *m;
                while ((m = api.class_get_methods(klass, &iter))) {
                    const char *mn = api.method_get_name(m);
                    uint32_t iflags = 0;
                    uint32_t mf = api.method_get_flags(m, &iflags);
                    auto *rt = api.method_get_return_type(m);
                    std::string rtn = type_name(api, rt);

                    // Method RVA: prefer the accessor, else read the pointer at
                    // offset 0 of MethodInfo (methodPointer — stable layout).
                    void *ptr = nullptr;
                    if (api.method_get_pointer) ptr = api.method_get_pointer(m);
                    else ptr = *reinterpret_cast<void *const *>(m);
                    uintptr_t va = reinterpret_cast<uintptr_t>(ptr);
                    uintptr_t rva = (api.base && va >= api.base) ? (va - api.base) : va;

                    std::string sig;
                    sig += rtn;
                    sig += " ";
                    sig += mn ? mn : "?";
                    sig += "(";
                    uint32_t pc = api.method_get_param_count(m);
                    for (uint32_t i = 0; i < pc; ++i) {
                        auto *pt = api.method_get_param(m, i);
                        const char *pname = api.method_get_param_name ? api.method_get_param_name(m, i) : nullptr;
                        if (i) sig += ", ";
                        sig += type_name(api, pt);
                        if (pname && *pname) { sig += " "; sig += pname; }
                    }
                    sig += ")";
                    out.put("    // RVA: 0x%" PRIxPTR " Offset: 0x0 VA: 0x%" PRIxPTR "\n", rva, va);
                    out.put("    %s%s %s { }\n", method_mods(mf).c_str(), rtn.c_str(), sig.substr(rtn.size() + 1).c_str());
                }
            }
            out.put("}\n\n");
        }
    }
}

// --------------------------------------------------------------------------
// bypass.h
// --------------------------------------------------------------------------
void write_bypass_h(const Il2CppApi &api, OutFile &out) {
    out.put("// ioctl-codex bypass.h — hook macros, offset table and bypass stubs.\n");
    out.put("// Auto-generated; regenerate after each game update.\n");
    out.put("#pragma once\n");
    out.put("#include <stdint.h>\n");
    out.put("#include <stddef.h>\n\n");
    out.put("#define IOCTL_CODEX_IL2CPP_BASE 0x%" PRIxPTR "ULL\n\n", api.base);

    out.put("// Resolve an RVA (from dump.cs) to a live address once the lib is mapped.\n");
    out.put("static inline void *codex_at(uintptr_t rva) {\n");
    out.put("    return (void *)(IOCTL_CODEX_IL2CPP_BASE + rva);\n");
    out.put("}\n\n");

    out.put("// Hook helper: install an inline hook on a resolved address.\n");
    out.put("//   codex_hook(codex_at(0x123456), my_fn, &orig_fn);\n");
    out.put("#ifdef __cplusplus\nextern \"C\" {\n#endif\n");
    out.put("int codex_hook(void *target, void *replace, void **origin);\n");
    out.put("int codex_unhook(void *target);\n");
    out.put("#ifdef __cplusplus\n}\n#endif\n\n");

    out.put("// ---- common detection bypass stubs --------------------------------\n");
    out.put("// Define these in your bypass translation unit and hook the matching\n");
    out.put("// game/JNI methods to them. They always report a clean device.\n");
    out.put("static inline int codex_bypass_isRooted(void)   { return 0; }\n");
    out.put("static inline int codex_bypass_isEmulator(void) { return 0; }\n");
    out.put("static inline int codex_bypass_isDebugger(void) { return 0; }\n");
    out.put("static inline int codex_bypass_isHooked(void)   { return 0; }\n");
    out.put("static inline int codex_bypass_isTampered(void) { return 0; }\n\n");

    out.put("// ---- method RVA table ---------------------------------------------\n");
    out.put("// class::method -> RVA (add IOCTL_CODEX_IL2CPP_BASE for the VA)\n");

    auto *domain = api.domain_get();
    if (!domain) return;
    size_t asm_count = 0;
    const Il2CppApi::Il2CppAssembly **asms = api.domain_get_assemblies(domain, &asm_count);
    if (!asms) return;
    for (size_t a = 0; a < asm_count; ++a) {
        auto *image = api.assembly_get_image(asms[a]);
        if (!image) continue;
        size_t cls_count = api.image_get_class_count(image);
        for (size_t c = 0; c < cls_count; ++c) {
            auto *klass = api.image_get_class(image, c);
            if (!klass) continue;
            const char *name = api.class_get_name(klass);
            const char *ns = api.class_get_namespace(klass);
            std::string full = join_ns(ns, name);
            // sanitize for a C identifier
            for (char &ch : full) if (!isalnum((unsigned char)ch) && ch != '_') ch = '_';
            if (api.class_get_methods) {
                void *iter = nullptr;
                const Il2CppApi::MethodInfo *m;
                while ((m = api.class_get_methods(klass, &iter))) {
                    const char *mn = api.method_get_name(m);
                    void *ptr = api.method_get_pointer ? api.method_get_pointer(m)
                                                       : *reinterpret_cast<void *const *>(m);
                    uintptr_t va = reinterpret_cast<uintptr_t>(ptr);
                    uintptr_t rva = (api.base && va >= api.base) ? (va - api.base) : va;
                    std::string mid = mn ? mn : "m";
                    for (char &ch : mid) if (!isalnum((unsigned char)ch) && ch != '_') ch = '_';
                    out.put("#define RVA_%s__%s 0x%" PRIxPTR "ULL\n", full.c_str(), mid.c_str(), rva);
                }
            }
        }
    }
}

// --------------------------------------------------------------------------
// il2cpp.h
// --------------------------------------------------------------------------
void write_il2cpp_h(const Il2CppApi &api, OutFile &out) {
    out.put("// ioctl-codex il2cpp.h — runtime type/struct definitions with offsets.\n");
    out.put("// Auto-generated from the live IL2CPP domain.\n");
    out.put("#pragma once\n");
    out.put("#include <stdint.h>\n\n");
    out.put("// Core runtime types (forward declarations; matches il2cpp-class-internals).\n");
    out.put("typedef struct Il2CppClass Il2CppClass;\n");
    out.put("typedef struct Il2CppObject { Il2CppClass *klass; void *monitor; } Il2CppObject;\n");
    out.put("typedef struct MethodInfo MethodInfo;\n");
    out.put("typedef struct FieldInfo FieldInfo;\n\n");

    out.put("// Managed object layout: every reference type starts with these two\n");
    out.put("// pointers. Field offsets below are relative to the END of this header\n");
    out.put("// for reference types, or to the object start for value types.\n");
    out.put("#define IOCTL_CODEX_OBJECT_HEADER (sizeof(void*) * 2)\n\n");

    auto *domain = api.domain_get();
    if (!domain) return;
    size_t asm_count = 0;
    const Il2CppApi::Il2CppAssembly **asms = api.domain_get_assemblies(domain, &asm_count);
    if (!asms) return;

    for (size_t a = 0; a < asm_count; ++a) {
        auto *image = api.assembly_get_image(asms[a]);
        if (!image) continue;
        size_t cls_count = api.image_get_class_count(image);
        for (size_t c = 0; c < cls_count; ++c) {
            auto *klass = api.image_get_class(image, c);
            if (!klass) continue;
            if (api.class_is_enum(klass)) continue;      // enums are just ints
            const char *name = api.class_get_name(klass);
            const char *ns = api.class_get_namespace(klass);
            std::string full = join_ns(ns, name);
            for (char &ch : full) if (!isalnum((unsigned char)ch) && ch != '_') ch = '_';

            bool vt = api.class_is_valuetype(klass);
            out.put("// %s (%s)\n", join_ns(ns, name).c_str(), vt ? "value type" : "reference type");
            out.put("typedef struct %s {\n", full.c_str());
            if (!vt) out.put("    Il2CppObject _header; // 0x0\n");
            bool any = false;
            if (api.class_get_fields) {
                void *iter = nullptr;
                Il2CppApi::FieldInfo *fld;
                while ((fld = api.class_get_fields(klass, &iter))) {
                    uint32_t ff = api.field_get_flags(fld);
                    if (ff & 0x0010) continue; // skip static (no instance offset)
                    const char *fn = api.field_get_name(fld);
                    size_t off = api.field_get_offset(fld);
                    std::string tn = type_name(api, api.field_get_type(fld));
                    // Map primitive C# types to C; everything else to a pointer.
                    const char *ct = "void*";
                    if (tn == "System.Int32") ct = "int32_t";
                    else if (tn == "System.UInt32") ct = "uint32_t";
                    else if (tn == "System.Int64") ct = "int64_t";
                    else if (tn == "System.UInt64") ct = "uint64_t";
                    else if (tn == "System.Int16") ct = "int16_t";
                    else if (tn == "System.UInt16") ct = "uint16_t";
                    else if (tn == "System.Byte") ct = "uint8_t";
                    else if (tn == "System.SByte") ct = "int8_t";
                    else if (tn == "System.Single") ct = "float";
                    else if (tn == "System.Double") ct = "double";
                    else if (tn == "System.Boolean") ct = "uint8_t";
                    else if (tn == "System.Char") ct = "uint16_t";
                    out.put("    %s %s; // 0x%zX : %s\n", ct, fn ? fn : "f", off, tn.c_str());
                    any = true;
                }
            }
            if (!any) out.put("    uint8_t _pad[8];\n");
            out.put("} %s;\n\n", full.c_str());
        }
    }
}

// --------------------------------------------------------------------------
// Directory + file plumbing
// --------------------------------------------------------------------------
bool ensure_dir(const std::string &path) {
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
    }
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

std::string version_string(const IoctlCodexCtx *ctx) {
    if (ctx->version_name && *ctx->version_name) return ctx->version_name;
    if (ctx->version_code > 0) { char b[32]; snprintf(b, sizeof(b), "%lld", (long long)ctx->version_code); return b; }
    return "unknown";
}

} // namespace

extern "C" int ioctl_codex_dumper_run(const IoctlCodexCtx *ctx) {
    g_ctx = ctx;
    if (!ctx) return -1;

    logmsg(IOCTL_CODEX_LOG_INFO, "starting IL2CPP dump for %s",
           ctx->package_name ? ctx->package_name : "?");

    Il2CppApi api;
    if (!api.load()) return -2;

    std::string ver = version_string(ctx);
    std::string pkg = ctx->package_name ? ctx->package_name : "unknown";

    std::string stage = ctx->stage_root ? ctx->stage_root : "/data/local/tmp/codex-dump";
    std::string dir = stage + "/" + pkg + "/" + ver;
    if (!ensure_dir(dir)) {
        logmsg(IOCTL_CODEX_LOG_ERROR, "cannot create stage dir %s", dir.c_str());
        return -3;
    }

    OutFile dump, bypass, il2h;
    if (!dump.open(dir + "/dump.cs") || !bypass.open(dir + "/bypass.h") || !il2h.open(dir + "/il2cpp.h")) {
        logmsg(IOCTL_CODEX_LOG_ERROR, "cannot open output files in %s", dir.c_str());
        return -4;
    }

    logmsg(IOCTL_CODEX_LOG_INFO, "writing dump.cs ...");
    write_dump_cs(api, dump);
    dump.close();
    logmsg(IOCTL_CODEX_LOG_INFO, "writing bypass.h ...");
    write_bypass_h(api, bypass);
    bypass.close();
    logmsg(IOCTL_CODEX_LOG_INFO, "writing il2cpp.h ...");
    write_il2cpp_h(api, il2h);
    il2h.close();

    // Publish to /sdcard via the root companion.
    std::string rel = pkg + "/" + ver;
    int rc = 0;
    if (ctx->publish_dir) {
        rc = ctx->publish_dir(dir.c_str(), rel.c_str());
    } else {
        logmsg(IOCTL_CODEX_LOG_WARN, "no publish_dir service; files left in %s", dir.c_str());
        rc = -5;
    }

    if (rc == 0) {
        logmsg(IOCTL_CODEX_LOG_INFO, "dump complete -> %s/%s",
               ctx->dump_root ? ctx->dump_root : "/sdcard/panxcz-dumper", rel.c_str());
    } else {
        logmsg(IOCTL_CODEX_LOG_WARN, "publish failed (rc=%d); staged at %s", rc, dir.c_str());
    }
    return 0;
}

extern "C" int ioctl_codex_custom_init(const IoctlCodexCtx *) {
    // The dumper does nothing on load; it runs on demand from the Unity tab.
    return 0;
}

extern "C" void ioctl_codex_custom_deinit(void) {
    // Nothing to tear down (we hold no allocations across runs).
}
