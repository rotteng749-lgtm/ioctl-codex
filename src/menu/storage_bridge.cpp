// src/menu/storage_bridge.cpp — the stage -> publish storage bridge.
//
// Problem: this code runs inside the target's sandbox (untrusted_app). It can
// write its own data dir but CANNOT write /sdcard/panxcz-dumper (scoped
// storage / SELinux denies untrusted_app -> media_rw on most devices).
//
// Solution: write everything to <app_data_dir>/codex/dump/... (allowed), then
// ask the module's ROOT companion to move the finished tree to the final
// destination and chmod/chown it for media_rw. The companion socket was opened
// by the Zygisk module in preAppSpecialize (the only time connectCompanion()
// works) and handed to us as g_target.root_fd.
//
// The protocol is line-based (see module.cpp root_companion):
//   MOVE <src>\t<dst>\t<mode>\n  -> "OK\n" | "ERR ...\n"
//   MKDIR <path>\n               -> "OK\n" | "ERR ...\n"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "menu.h"

namespace codex {

std::string root_request(const std::string &line) {
    if (g_target.root_fd < 0) return {};
    std::string req = line;
    if (req.empty() || req.back() != '\n') req.push_back('\n');

    // Send the full request.
    size_t off = 0;
    while (off < req.size()) {
        ssize_t w = send(g_target.root_fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (w <= 0) return {};
        off += (size_t)w;
    }

    // Read until newline.
    std::string reply;
    char c;
    for (;;) {
        ssize_t n = recv(g_target.root_fd, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') break;
        reply.push_back(c);
        if (reply.size() > 4096) break;
    }
    return reply;
}

int storage_publish_dir(const char *stage_path, const char *final_rel) {
    if (!stage_path || !final_rel) return -1;

    std::string root = g_target.dump_root.empty() ? "/sdcard/panxcz-dumper" : g_target.dump_root;
    std::string dst = root + "/" + final_rel;

    if (g_target.root_fd < 0) {
        ilog("storage", "no root companion; leaving dump staged at %s", stage_path);
        return -2;
    }

    // Make sure the parent exists (root does mkdir -p too, but be explicit).
    std::string parent = dst.substr(0, dst.find_last_of('/'));
    root_request("MKDIR " + parent);

    std::string req = "MOVE " + std::string(stage_path) + "\t" + dst + "\t0777";
    std::string reply = root_request(req);
    if (reply.rfind("OK", 0) == 0) {
        ilog("storage", "published -> %s", dst.c_str());
        return 0;
    }
    ilog("storage", "publish failed: %s", reply.empty() ? "(no reply)" : reply.c_str());
    return -3;
}

} // namespace codex
