// Sandstorm Jekyll App
// Copyright (c) 2014, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Hack around stdlib bug with C++14.
#include <initializer_list>  // force libstdc++ to include its config
#undef _GLIBCXX_HAVE_GETS    // correct broken config
// End hack.

#include <kj/main.h>
#include <kj/debug.h>
#include <kj/io.h>
#include <kj/async-io.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.capnp.h>
#include <capnp/schema.h>
#include <unistd.h>
#include <map>
#include <unordered_map>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <algorithm>

#include <sandstorm/grain.capnp.h>
#include <sandstorm/web-session.capnp.h>
#include <sandstorm/hack-session.capnp.h>

namespace ssjekyll {

#if __QTCREATOR
#define KJ_MVCAP(var) var
// QtCreator dosen't understand C++14 syntax yet.
#else
#define KJ_MVCAP(var) var = ::kj::mv(var)
// Capture the given variable by move.  Place this in a lambda capture list.  Requires C++14.
//
// TODO(cleanup):  Move to libkj.
#endif

typedef unsigned int uint;
typedef unsigned char byte;

using sandstorm::WebSession;
using sandstorm::UserInfo;
using sandstorm::SessionContext;
using sandstorm::UiView;
using sandstorm::SandstormApi;
using sandstorm::HttpStatusDescriptor;

kj::AutoCloseFd raiiOpen(kj::StringPtr name, int flags, mode_t mode = 0666) {
  int fd;
  KJ_SYSCALL(fd = open(name.cStr(), flags, mode), name);
  return kj::AutoCloseFd(fd);
}

size_t getFileSize(int fd, kj::StringPtr filename) {
  struct stat stats;
  KJ_SYSCALL(fstat(fd, &stats));
  KJ_REQUIRE(S_ISREG(stats.st_mode), "Not a regular file.", filename);
  return stats.st_size;
}

void writeFile(kj::StringPtr filename, kj::StringPtr content) {
  kj::FdOutputStream(raiiOpen(filename, O_WRONLY | O_CREAT | O_EXCL))
      .write(reinterpret_cast<const byte*>(content.begin()), content.size());
}

kj::Vector<kj::String> listDirectory(kj::StringPtr dirname) {
  kj::Vector<kj::String> entries;

  DIR* dir = opendir(dirname.cStr());
  if (dir == nullptr) {
    KJ_FAIL_SYSCALL("opendir", errno, dirname);
  }
  KJ_DEFER(closedir(dir));

  for (;;) {
    errno = 0;
    struct dirent* entry = readdir(dir);
    if (entry == nullptr) {
      int error = errno;
      if (error == 0) {
        break;
      } else {
        KJ_FAIL_SYSCALL("readdir", error, dirname);
      }
    }

    kj::StringPtr name = entry->d_name;
    if (name != "." && name != "..") {
      entries.add(kj::heapString(entry->d_name));
    }
  }

  return entries;
}

kj::String dirnamePath(kj::StringPtr diskPath) {
  KJ_IF_MAYBE(lastSlash, diskPath.findLast('/')) {
    // Strip off last component of path.
    kj::ArrayPtr<const char> parent = diskPath.slice(0, *lastSlash);

    // Strip off any further trailing slashes.
    while (parent.size() > 0 && parent[parent.size() - 1] == '/') {
      parent = parent.slice(0, parent.size() - 1);
    }

    return kj::heapString(parent);
  } else {
    KJ_FAIL_REQUIRE("Disk path had no slashes in it");
  }
}

template<class... Args>
void callProcessWithPipe(kj::ArrayPtr<const kj::byte> stdinData,
                         kj::StringPtr progname, Args&&... argv) {
  int pipeFds[2];
  KJ_SYSCALL(pipe2(pipeFds, O_CLOEXEC));
  kj::AutoCloseFd pipeIn(pipeFds[0]);
  kj::AutoCloseFd pipeOut(pipeFds[1]);

  pid_t pid;
  KJ_SYSCALL(pid = fork());

  if (pid > 0) {
    // After we've written to the pipe, *or* if writing to the pipe throws, we need to waitpid().
    // TODO(cleanup): Someday when we have a nice KJ class encapsulating child processes, this can
    //   be less ugly.
    KJ_DEFER({
      pipeOut = nullptr;
      int status;

      // Use non-fatal asserts here in case this is called during exception unwind.
      KJ_SYSCALL(waitpid(pid, &status, 0)) { return; }
      KJ_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "child process failed", progname) { return; }
    });

    pipeIn = nullptr;
    kj::FdOutputStream(kj::mv(pipeOut)).write(stdinData.begin(), stdinData.size());
  } else {
    KJ_SYSCALL(dup2(pipeIn, STDIN_FILENO));
    KJ_SYSCALL(execlp(progname.cStr(), kj::fwd<Args>(argv)..., (const char*)nullptr));
    KJ_UNREACHABLE;
  }
}

void ensureParentDirectoryCreated(kj::StringPtr diskPath) {
  KJ_IF_MAYBE(lastSlash, diskPath.findLast('/')) {
    // Strip off last component of path.
    kj::ArrayPtr<const char> parent = diskPath.slice(0, *lastSlash);

    // Strip off any further trailing slashes.
    while (parent.size() > 0 && parent[parent.size() - 1] == '/') {
      parent = parent.slice(0, parent.size() - 1);
    }

    if (parent.size() > 0) {
      auto parentStr = kj::heapString(parent);
      if (access(parentStr.cStr(), F_OK) != 0) {
        ensureParentDirectoryCreated(parentStr);
        KJ_SYSCALL(mkdir(parentStr.cStr(), 0777));
      }
    }
  }
}

void recursivelyDelete(kj::StringPtr diskPath) {
  struct stat stats;
  KJ_SYSCALL(lstat(diskPath.cStr(), &stats));
  if (S_ISDIR(stats.st_mode)) {
    for (auto& entry: listDirectory(diskPath)) {
      recursivelyDelete(kj::str(diskPath, '/', entry));
    }
    KJ_SYSCALL(rmdir(diskPath.cStr()));
  } else {
    KJ_SYSCALL(unlink(diskPath.cStr()));
  }
}

// =======================================================================================
// Background Jekyll

pid_t runJekyll(kj::StringPtr config, kj::StringPtr outDir, bool watch) {
  pid_t child = fork();
  if (child == 0) {
    // This is the child.
    close(3);  // Close Sandstorm API socket.

    KJ_SYSCALL(execlp(
        "jekyll", "jekyll", "build", "-s", "/var/src", "-d", outDir.cStr(),
        "--config", config.cStr(),
        watch ? "-w" : (const char*)nullptr, (const char*)nullptr));
    KJ_UNREACHABLE;
  }

  return child;
}

pid_t jekyllPreviewPid = 0;
pid_t jekyllPublishPid = 0;

void restartJekyllPreview() {
  if (jekyllPreviewPid != 0) {
    KJ_SYSCALL(kill(jekyllPreviewPid, SIGTERM));
    int status;
    KJ_SYSCALL(waitpid(jekyllPreviewPid, &status, 0));
  }

  jekyllPreviewPid = runJekyll("/var/src/_config_preview.yaml", "/var/preview", true);
}

void doJekyllPublish() {
  if (jekyllPublishPid != 0) {
    // Reap previous publish task (and prevent two publishes from happening at the same time).
    KJ_SYSCALL(kill(jekyllPublishPid, SIGTERM));
    int status;
    KJ_SYSCALL(waitpid(jekyllPublishPid, &status, 0));
  }

  jekyllPublishPid = runJekyll("/var/src/_config_published.yaml", "/var/published", false);
}

// =======================================================================================
// WebSession

class WebSessionImpl final: public WebSession::Server {
public:
  WebSessionImpl(UserInfo::Reader userInfo, SessionContext::Client context,
                 WebSession::Params::Reader params)
      : context(kj::mv(context)),
        userDisplayName(kj::heapString(userInfo.getDisplayName().getDefaultText())),
        basePath(kj::heapString(params.getBasePath())),
        userAgent(kj::heapString(params.getUserAgent())),
        acceptLanguages(kj::strArray(params.getAcceptableLanguages(), ",")) {}

  kj::Promise<void> get(GetContext context) override {
    auto path = context.getParams().getPath();
    if (path.startsWith("file/")) {
      return readFile(kj::str("/var/src/", path.slice(strlen("file/"))), context);
    } else if (path == "file") {
      auto text = dir2json("/var/src").flatten();
      auto content = context.getResults(capnp::MessageSize {
          text.size() / sizeof(capnp::word) + 32, 0 }).initContent();
      content.setStatusCode(WebSession::Response::SuccessCode::OK);
      content.setMimeType("application/json");
      content.getBody().setBytes(kj::arrayPtr(
          reinterpret_cast<const byte*>(text.begin()), text.size()));
      return kj::READY_NOW;
    } else if (path.startsWith("preview/")) {
      return readFile(kj::str("/var/", path), context);
    } else if (path == "preview") {
      auto redirect = context.getResults().initRedirect();
      redirect.setIsPermanent(true);
      redirect.setSwitchToGet(true);
      redirect.setLocation("/preview/");
      return kj::READY_NOW;
    } else if (path == "publicId") {
      return this->context.castAs<HackSessionContext>().getPublicIdRequest().send()
          .then([context](auto&& response) mutable {
        auto text = kj::str("{ \"publicId\": \"", response.getPublicId(), "\",\n"
                            "  \"hostname\": \"", response.getHostname(), "\" }");
        auto content = context.getResults(capnp::MessageSize {
            text.size() / sizeof(capnp::word) + 32, 0 }).initContent();
        content.setStatusCode(WebSession::Response::SuccessCode::OK);
        content.setMimeType("application/json");
        content.getBody().setBytes(kj::arrayPtr(
            reinterpret_cast<const byte*>(text.begin()), text.size()));
      });
    } else {
      return readFile(kj::str("/client/", path), context);
    }
  }

  kj::Promise<void> post(PostContext context) override {
    auto params = context.getParams();
    auto path = params.getPath();

    if (path == "reboot") {
      restartJekyllPreview();
      auto content = context.getResults(capnp::MessageSize { 32, 0 }).initContent();
      content.setStatusCode(WebSession::Response::SuccessCode::OK);
      content.setMimeType("text/plain");
      content.getBody().setBytes(kj::arrayPtr(reinterpret_cast<const byte*>("ok"), 2));
      return kj::READY_NOW;
    } else if (path == "publish") {
      doJekyllPublish();
      auto content = context.getResults(capnp::MessageSize { 32, 0 }).initContent();
      content.setStatusCode(WebSession::Response::SuccessCode::OK);
      content.setMimeType("text/plain");
      content.getBody().setBytes(kj::arrayPtr(reinterpret_cast<const byte*>("ok"), 2));
      return kj::READY_NOW;
    } else {
      KJ_FAIL_REQUIRE("Invalid POST location.");
    }
  }

  kj::Promise<void> put(PutContext context) override {
    auto params = context.getParams();
    auto path = params.getPath();
    auto content = params.getContent().getContent();

    if (path.startsWith("archive/")) {
      auto archive = path.slice(strlen("archive/"));

      if (!archive.endsWith(".tar")) {
        KJ_FAIL_REQUIRE("Only .tar bundles are supported at the moment.");
      }

      auto diskPath = kj::str("/var/src/", archive);
      ensureParentDirectoryCreated(diskPath);
      auto outDir = dirnamePath(diskPath);

      callProcessWithPipe(content, "tar", "tar", "x", "-C", outDir.cStr());
    } else if (path.startsWith("file/")) {
      auto diskPath = kj::str("/var/src/", path.slice(strlen("file/")));
      ensureParentDirectoryCreated(diskPath);

      kj::FdOutputStream(raiiOpen(diskPath, O_WRONLY | O_TRUNC | O_CREAT))
          .write(content.begin(), content.size());
    } else {
      KJ_FAIL_REQUIRE("Invalid PUT location.");
    }

    auto responseContent = context.getResults(capnp::MessageSize { 32, 0 }).initContent();
    responseContent.setStatusCode(WebSession::Response::SuccessCode::OK);
    responseContent.setMimeType("text/plain");
    responseContent.getBody().setBytes(kj::arrayPtr(reinterpret_cast<const byte*>("ok"), 2));

    return kj::READY_NOW;
  }

  kj::Promise<void> delete_(DeleteContext context) override {
    auto params = context.getParams();
    auto path = params.getPath();

    KJ_REQUIRE(path.startsWith("file/"), "Invalid DELETE location.");

    auto diskPath = kj::str("/var/src/", path.slice(strlen("file/")));

    recursivelyDelete(diskPath);

    auto responseContent = context.getResults(capnp::MessageSize { 32, 0 }).initContent();
    responseContent.setStatusCode(WebSession::Response::SuccessCode::OK);
    responseContent.setMimeType("text/plain");
    responseContent.getBody().setBytes(kj::arrayPtr(reinterpret_cast<const byte*>("ok"), 2));

    return kj::READY_NOW;
  }

//  kj::Promise<void> openWebSocket(OpenWebSocketContext context) override {
//    // We could perhaps alert on inotify on the preview directory...
//  }

private:
  SessionContext::Client context;
  kj::String userDisplayName;
  kj::String basePath;
  kj::String userAgent;
  kj::String acceptLanguages;

  kj::Promise<void> readFile(kj::String filename, GetContext context) {
    if (filename.endsWith("/")) {
      filename = kj::str(filename, "index.html");
    } else {
      // TODO:  check if directory, redirect to add '/'
    }

    auto fd = raiiOpen(filename, O_RDONLY);
    auto size = getFileSize(fd, filename);
    kj::FdInputStream stream(kj::mv(fd));
    auto response = context.getResults(capnp::MessageSize { size + 32, 0 });
    auto content = response.initContent();
    content.setStatusCode(WebSession::Response::SuccessCode::OK);

    if (filename.endsWith(".html")) {
      content.setMimeType("text/html; charset=UTF-8");
    } else if (filename.endsWith(".md")) {
      content.setMimeType("text/x-markdown; charset=UTF-8");
    } else if (filename.endsWith(".js")) {
      content.setMimeType("text/javascript; charset=UTF-8");
    } else if (filename.endsWith(".css")) {
      content.setMimeType("text/css; charset=UTF-8");
    } else if (filename.endsWith(".yaml")) {
      content.setMimeType("text/yaml; charset=UTF-8");
    } else if (filename.endsWith(".png")) {
      content.setMimeType("image/png");
    } else if (filename.endsWith(".gif")) {
      content.setMimeType("image/gif");
    } else if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) {
      content.setMimeType("image/jpeg");
    } else {
      content.setMimeType("text/plain; charset=UTF-8");
    }

    stream.read(content.getBody().initBytes(size).begin(), size);
    return kj::READY_NOW;
  }

  kj::StringTree dir2json(kj::StringPtr dirname) {
    auto entries = listDirectory(dirname);

    auto sorted = KJ_MAP(e, entries) -> kj::StringPtr { return e; };
    std::sort(sorted.begin(), sorted.end());

    auto children = kj::StringTree(KJ_MAP(entry, sorted) {
      auto full = kj::str(dirname, '/', entry);
      struct stat stats;
      KJ_SYSCALL(stat(full.cStr(), &stats));
      auto quotedName = kj::str('"', entry, '"');
      if (S_ISDIR(stats.st_mode)) {
        return kj::strTree(quotedName, ":", dir2json(full));
      } else {
        return kj::strTree(quotedName, ":", stats.st_size);
      }
    }, ",");
    return kj::strTree("{", kj::mv(children), "}");
  }
};

class UiViewImpl final: public UiView::Server {
public:
//  kj::Promise<void> getViewInfo(GetViewInfoContext context) override;

  kj::Promise<void> newSession(NewSessionContext context) override {
    auto params = context.getParams();

    KJ_REQUIRE(params.getSessionType() == capnp::typeId<WebSession>(),
               "Unsupported session type.");

    context.getResults(capnp::MessageSize {2, 1}).setSession(
        kj::heap<WebSessionImpl>(params.getUserInfo(), params.getContext(),
                                 params.getSessionParams().getAs<WebSession::Params>()));

    return kj::READY_NOW;
  }
};

class SsJekyllMain {
public:
  SsJekyllMain(kj::ProcessContext& context): context(context), ioContext(kj::setupAsyncIo()) {}

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "Sandstorm-Jekyll Controller",
                           "Intended to be run as the root process of a Sandstorm app.")
        .addOption({'i'}, KJ_BIND_METHOD(*this, init), "Initialize a new grain.")
        .callAfterParsing(KJ_BIND_METHOD(*this, run))
        .build();
  }

  class Restorer: public capnp::SturdyRefRestorer<capnp::AnyPointer> {
  public:
    explicit Restorer(capnp::Capability::Client&& defaultCap)
        : defaultCap(kj::mv(defaultCap)) {}

    capnp::Capability::Client restore(capnp::AnyPointer::Reader ref) override {
      if (ref.isNull()) {
        return defaultCap;
      }
      KJ_FAIL_ASSERT("Unknown ref.");
    }

  private:
    capnp::Capability::Client defaultCap;
  };

  kj::MainBuilder::Validity init() {
    KJ_SYSCALL(mkdir("/var/src", 0777));
    KJ_SYSCALL(mkdir("/var/src/_layouts", 0777));
    KJ_SYSCALL(mkdir("/var/preview", 0777));
    KJ_SYSCALL(mkdir("/var/published", 0777));

    writeFile("/var/src/_config_preview.yaml",
        "# Config used to generate the preview pane.\n"
        "# The preview is continuously rebuilt while you edit.\n"
        "\n"
        "baseurl: /preview/  # Don't change this.\n"
        "\n"
        "# Add your own config below, but don't forget to update\n"
        "# _config_published.yaml too.\n");
    writeFile("/var/src/_config_published.yaml",
        "# Config used when you publish the live site.\n"
        "\n"
        "baseurl: /  # Note:  Different from preview.\n"
        "\n"
        "# Add your own config below, but don't forget to update\n"
        "# _config_preview.yaml too, or your preview pane will\n"
        "# look wrong.\n");
    writeFile("/var/src/index.md",
        "---\n"
        "layout: page\n"
        "title: My Site\n"
        "---\n"
        "\n"
        "# Hello World!\n");
    writeFile("/var/src/_layouts/page.html",
        "<html>\n"
        "<body>\n"
        "{{ content }}\n"
        "</body>\n"
        "</html>\n");

    return true;
  }

  kj::MainBuilder::Validity run() {
    // ssjekyll predates /var/www becoming the special directory for serving static content, and
    // instead uses /var/published. We create a symlink to work around this. We can't do this in
    // init() since we want grains created with the old version to be upgradable. We ignore any
    // errors because it's probably just an EEXIST.
    symlink("published", "/var/www");

    restartJekyllPreview();

    auto stream = ioContext.lowLevelProvider->wrapSocketFd(3);
    capnp::TwoPartyVatNetwork network(*stream, capnp::rpc::twoparty::Side::CLIENT);
    Restorer restorer(kj::heap<UiViewImpl>());
    auto rpcSystem = capnp::makeRpcServer(network, restorer);

    // Get the SandstormApi by restoring a null SturdyRef.
    // TODO(soon):  We don't use this, but for some reason the connection doesn't come up if we
    //   don't do this restore.  Cap'n Proto bug?  v8capnp bug?  Shell bug?
    {
      capnp::MallocMessageBuilder message;
      capnp::rpc::SturdyRef::Builder ref = message.getRoot<capnp::rpc::SturdyRef>();
      auto hostId = ref.getHostId().initAs<capnp::rpc::twoparty::SturdyRefHostId>();
      hostId.setSide(capnp::rpc::twoparty::Side::SERVER);
      SandstormApi::Client api = rpcSystem.restore(
          hostId, ref.getObjectId()).castAs<SandstormApi>();
    }

    kj::NEVER_DONE.wait(ioContext.waitScope);
  }

private:
  kj::ProcessContext& context;
  kj::AsyncIoContext ioContext;
};

}  // namespace ssjekyll

KJ_MAIN(ssjekyll::SsJekyllMain)
