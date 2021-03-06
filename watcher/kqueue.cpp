/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include "InMemoryView.h"
#include <array>

#ifdef HAVE_KQUEUE
#if !defined(O_EVTONLY)
# define O_EVTONLY O_RDONLY
#endif

namespace {

// This just holds a descriptor open, closing it when it is destroyed.
// It's not a general purpose file descriptor wrapper.
struct FileDescriptor {
  int fd;

  explicit FileDescriptor(int fd) : fd(fd) {}

  FileDescriptor() : fd(-1) {}

  // No copying
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept : fd(other.fd) {
    other.fd = -1;
  }
  FileDescriptor& operator=(FileDescriptor&& other) {
    reset();
    fd = other.fd;
    other.fd = -1;
    return *this;
  }

  void reset() {
    if (fd != -1) {
      w_log(W_LOG_DBG, "KQ close fd=%d\n", fd);
      close(fd);
    }
  }

  ~FileDescriptor() {
    reset();
  }
};
}

struct KQueueWatcher : public Watcher {
  int kq_fd{-1};
  int terminatePipe_[2]{-1, -1};

  struct maps {
    std::unordered_map<w_string, FileDescriptor> name_to_fd;
    /* map of active watch descriptor to name of the corresponding item */
    std::unordered_map<int, w_string> fd_to_name;

    explicit maps(json_int_t sizeHint) {
      name_to_fd.reserve(sizeHint);
      fd_to_name.reserve(sizeHint);
    }
  };
  watchman::Synchronized<maps> maps_;

  struct kevent keventbuf[WATCHMAN_BATCH_LIMIT];

  explicit KQueueWatcher(w_root_t* root);
  ~KQueueWatcher();

  struct watchman_dir_handle* startWatchDir(
      const std::shared_ptr<w_root_t>& root,
      struct watchman_dir* dir,
      struct timeval now,
      const char* path) override;

  bool startWatchFile(struct watchman_file* file) override;

  bool consumeNotify(
      const std::shared_ptr<w_root_t>& root,
      PendingCollection::LockedPtr& coll) override;

  bool waitNotify(int timeoutms) override;
  void signalThreads() override;
};

static const struct flag_map kflags[] = {
    {NOTE_DELETE, "NOTE_DELETE"},
    {NOTE_WRITE, "NOTE_WRITE"},
    {NOTE_EXTEND, "NOTE_EXTEND"},
    {NOTE_ATTRIB, "NOTE_ATTRIB"},
    {NOTE_LINK, "NOTE_LINK"},
    {NOTE_RENAME, "NOTE_RENAME"},
    {NOTE_REVOKE, "NOTE_REVOKE"},
    {0, nullptr},
};

KQueueWatcher::KQueueWatcher(w_root_t* root)
    : Watcher("kqueue", 0),
      maps_(maps(root->config.getInt(CFG_HINT_NUM_DIRS, HINT_NUM_DIRS))) {
  if (pipe(terminatePipe_)) {
    throw std::system_error(
        errno,
        std::system_category(),
        std::string("pipe error: ") + strerror(errno));
  }
  w_set_cloexec(terminatePipe_[0]);
  w_set_cloexec(terminatePipe_[1]);

  kq_fd = kqueue();
  if (kq_fd == -1) {
    throw std::system_error(
        errno,
        std::system_category(),
        std::string("kqueue error: ") + strerror(errno));
  }
  w_set_cloexec(kq_fd);
}

KQueueWatcher::~KQueueWatcher() {
  if (kq_fd != -1) {
    close(kq_fd);
  }
  if (terminatePipe_[0] != -1) {
    close(terminatePipe_[0]);
  }
  if (terminatePipe_[1] != -1) {
    close(terminatePipe_[1]);
  }
}

bool KQueueWatcher::startWatchFile(struct watchman_file* file) {
  struct kevent k;

  auto full_name = w_dir_path_cat_str(file->parent, file->getName());
  {
    auto rlock = maps_.rlock();
    if (rlock->name_to_fd.find(full_name) != rlock->name_to_fd.end()) {
      // Already watching it
      return true;
    }
  }

  w_log(W_LOG_DBG, "watch_file(%s)\n", full_name.c_str());

  FileDescriptor fdHolder(open(full_name.c_str(), O_EVTONLY | O_CLOEXEC));

  auto rawFd = fdHolder.fd;

  if (rawFd == -1) {
    watchman::log(
        watchman::ERR,
        "failed to open ",
        full_name,
        ", O_EVTONLY: ",
        strerror(errno),
        "\n");
    return false;
  }

  memset(&k, 0, sizeof(k));
  EV_SET(
      &k,
      rawFd,
      EVFILT_VNODE,
      EV_ADD | EV_CLEAR,
      NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_RENAME | NOTE_ATTRIB,
      0,
      (w_string_t*)full_name);

  {
    auto wlock = maps_.wlock();
    wlock->name_to_fd[full_name] = std::move(fdHolder);
    wlock->fd_to_name[rawFd] = full_name;
  }

  if (kevent(kq_fd, &k, 1, nullptr, 0, 0)) {
    watchman::log(
        watchman::DBG,
        "kevent EV_ADD file ",
        full_name,
        " failed: ",
        full_name.c_str(),
        strerror(errno),
        "\n");
    auto wlock = maps_.wlock();
    wlock->name_to_fd.erase(full_name);
    wlock->fd_to_name.erase(rawFd);
  } else {
    watchman::log(
        watchman::DBG, "kevent file ", full_name, " -> ", rawFd, "\n");
  }

  return true;
}

struct watchman_dir_handle* KQueueWatcher::startWatchDir(
    const std::shared_ptr<w_root_t>& root,
    struct watchman_dir* dir,
    struct timeval now,
    const char* path) {
  struct watchman_dir_handle *osdir;
  struct stat st, osdirst;
  struct kevent k;

  osdir = w_dir_open(path);
  if (!osdir) {
    handle_open_errno(root, dir, now, "opendir", errno, nullptr);
    return nullptr;
  }

  FileDescriptor fdHolder(open(path, O_NOFOLLOW | O_EVTONLY | O_CLOEXEC));
  auto rawFd = fdHolder.fd;

  if (rawFd == -1) {
    // directory got deleted between opendir and open
    handle_open_errno(root, dir, now, "open", errno, nullptr);
    w_dir_close(osdir);
    return nullptr;
  }
  if (fstat(rawFd, &st) == -1 || fstat(w_dir_fd(osdir), &osdirst) == -1) {
    // whaaa?
    w_log(W_LOG_ERR, "fstat on opened dir %s failed: %s\n", path,
        strerror(errno));
    root->scheduleRecrawl("fstat failed");
    w_dir_close(osdir);
    return nullptr;
  }

  if (st.st_dev != osdirst.st_dev || st.st_ino != osdirst.st_ino) {
    // directory got replaced between opendir and open -- at this point its
    // parent's being watched, so we let filesystem events take care of it
    handle_open_errno(root, dir, now, "open", ENOTDIR, nullptr);
    w_dir_close(osdir);
    return nullptr;
  }

  memset(&k, 0, sizeof(k));
  auto dir_name = dir->getFullPath();
  EV_SET(
      &k,
      rawFd,
      EVFILT_VNODE,
      EV_ADD | EV_CLEAR,
      NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_RENAME,
      0,
      SET_DIR_BIT((w_string_t*)dir_name));

  // Our mapping needs to be visible before we add it to the queue,
  // otherwise we can get a wakeup and not know what it is
  {
    auto wlock = maps_.wlock();
    wlock->name_to_fd[dir_name] = std::move(fdHolder);
    wlock->fd_to_name[rawFd] = dir_name;
  }

  if (kevent(kq_fd, &k, 1, nullptr, 0, 0)) {
    w_log(W_LOG_DBG, "kevent EV_ADD dir %s failed: %s",
        path, strerror(errno));

    auto wlock = maps_.wlock();
    wlock->name_to_fd.erase(dir_name);
    wlock->fd_to_name.erase(rawFd);
  } else {
    watchman::log(watchman::DBG, "kevent dir ", dir_name, " -> ", rawFd, "\n");
  }

  return osdir;
}

bool KQueueWatcher::consumeNotify(
    const std::shared_ptr<w_root_t>& root,
    PendingCollection::LockedPtr& coll) {
  int n;
  int i;
  struct timespec ts = { 0, 0 };
  struct timeval now;

  errno = 0;
  n = kevent(
      kq_fd,
      nullptr,
      0,
      keventbuf,
      sizeof(keventbuf) / sizeof(keventbuf[0]),
      &ts);
  w_log(
      W_LOG_DBG,
      "consume_kqueue: %s n=%d err=%s\n",
      root->root_path.c_str(),
      n,
      strerror(errno));
  if (root->inner.cancelled) {
    return 0;
  }

  gettimeofday(&now, nullptr);
  for (i = 0; n > 0 && i < n; i++) {
    uint32_t fflags = keventbuf[i].fflags;
    bool is_dir = IS_DIR_BIT_SET(keventbuf[i].udata);
    char flags_label[128];
    int fd = keventbuf[i].ident;

    w_expand_flags(kflags, fflags, flags_label, sizeof(flags_label));
    auto wlock = maps_.wlock();
    auto it = wlock->fd_to_name.find(fd);
    w_string path = it == wlock->fd_to_name.end() ? nullptr : it->second;
    if (!path) {
      // Was likely a buffered notification for something that we decided
      // to stop watching
      w_log(W_LOG_DBG,
          " KQ notif for fd=%d; flags=0x%x %s no ref for it in fd_to_name\n",
          fd, fflags, flags_label);
      continue;
    }

    w_log(
        W_LOG_DBG,
        " KQ fd=%d path %s [0x%x %s]\n",
        fd,
        path.data(),
        fflags,
        flags_label);
    if ((fflags & (NOTE_DELETE|NOTE_RENAME|NOTE_REVOKE))) {
      struct kevent k;

      if (w_string_equal(path, root->root_path)) {
        w_log(
            W_LOG_ERR,
            "root dir %s has been (re)moved [code 0x%x], canceling watch\n",
            root->root_path.c_str(),
            fflags);
        root->cancel();
        return 0;
      }

      // Remove our watch bits
      memset(&k, 0, sizeof(k));
      EV_SET(&k, fd, EVFILT_VNODE, EV_DELETE, 0, 0, nullptr);
      kevent(kq_fd, &k, 1, nullptr, 0, 0);
      wlock->name_to_fd.erase(path);
      wlock->fd_to_name.erase(fd);
    }

    coll->add(
        path, now, is_dir ? 0 : (W_PENDING_RECURSIVE | W_PENDING_VIA_NOTIFY));
  }

  return n > 0;
}

bool KQueueWatcher::waitNotify(int timeoutms) {
  int n;
  std::array<struct pollfd, 2> pfd;

  pfd[0].fd = kq_fd;
  pfd[0].events = POLLIN;
  pfd[1].fd = terminatePipe_[0];
  pfd[1].events = POLLIN;

  n = poll(pfd.data(), pfd.size(), timeoutms);

  if (n > 0) {
    if (pfd[1].revents) {
      // We were signalled via signalThreads
      return false;
    }
    return pfd[0].revents != 0;
  }
  return false;
}

void KQueueWatcher::signalThreads() {
  ignore_result(write(terminatePipe_[1], "X", 1));
}

static RegisterWatcher<KQueueWatcher> reg(
    "kqueue",
    -1 /* last resort on OS X */);

#endif // HAVE_KQUEUE

/* vim:ts=2:sw=2:et:
 */
