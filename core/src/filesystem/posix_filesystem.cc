/**
 * @file   posix_filesystem.cc
 *
 * @section LICENSE
 *
 * The MIT License
 *
 * @copyright Copyright (c) 2017 TileDB, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * @section DESCRIPTION
 *
 * This file includes definitions of POSIX filesystem functions.
 */

#include "posix_filesystem.h"
#include "constants.h"
#include "logger.h"
#include "utils.h"

#include <dirent.h>
#include <sys/mman.h>
#include <zlib.h>
#include <fstream>
#include <iostream>

namespace tiledb {

namespace posix {

Status create_dir(const std::string& path) {
  // If the directory does not exist, create it
  if (posix::is_dir(path)) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot create directory '") + path +
        "'; Directory already exists"));
  }
  if (mkdir(path.c_str(), S_IRWXU)) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot create directory '") + path + "'; " +
        strerror(errno)));
  }
  return Status::Ok();
}

std::string current_dir() {
  std::string dir = "";
  char* path = getcwd(nullptr, 0);
  if (path != nullptr) {
    dir = path;
    free(path);
  }
  return dir;
}

/*
  // TODO
Status delete_dir(const URI& uri) {
return delete_dir(uri.to_posix_path());
}

Status delete_dir(const std::string& path) {
// Get real path
std::string dirname_real = posix::real_dir(path);

// Delete the contents of the directory
std::string filename;
struct dirent* next_file;
DIR* dir = opendir(dirname_real.c_str());

if (dir == nullptr) {
return LOG_STATUS(Status::OSError(
    std::string("Cannot open directory; ") + strerror(errno)));
}

while ((next_file = readdir(dir))) {
if (!strcmp(next_file->d_name, ".") || !strcmp(next_file->d_name, ".."))
  continue;
filename = dirname_real + "/" + next_file->d_name;
if (remove(filename.c_str())) {
  return LOG_STATUS(Status::OSError(
      std::string("Cannot delete file; ") + strerror(errno)));
}
}

// Close directory
if (closedir(dir)) {
return LOG_STATUS(Status::OSError(
    std::string("Cannot close directory; ") + strerror(errno)));
}

// Remove directory
if (rmdir(dirname_real.c_str())) {
return LOG_STATUS(Status::OSError(
    std::string("Cannot delete directory; ") + strerror(errno)));
}
return Status::Ok();
}

*/

Status delete_file(const std::string& path) {
  if (remove(path.c_str())) {
    return LOG_STATUS(
        Status::OSError(std::string("Cannot delete file; ") + strerror(errno)));
  }
  return Status::Ok();
}

Status file_size(const std::string& path, uint64_t* size) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd == -1) {
    return LOG_STATUS(
        Status::OSError("Cannot get file size; File opening error"));
  }

  struct stat st;
  fstat(fd, &st);
  *size = st.st_size;

  close(fd);
  return Status::Ok();
}

bool is_dir(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string& path) {
  struct stat st;
  return (stat(path.c_str(), &st) == 0) && !S_ISDIR(st.st_mode);
}

void purge_dots_from_path(std::string* path) {
  // Trivial case
  if (path == nullptr)
    return;

  // Trivial case
  uint64_t path_size = path->size();
  if (path_size == 0 || *path == "file:///")
    return;

  assert(utils::starts_with(*path, "file:///"));

  // Tokenize
  const char* token_c_str = path->c_str() + 8;
  std::vector<std::string> tokens, final_tokens;
  std::string token;

  for (uint64_t i = 8; i < path_size; ++i) {
    if ((*path)[i] == '/') {
      (*path)[i] = '\0';
      token = token_c_str;
      if (!token.empty())
        tokens.push_back(token);
      token_c_str = path->c_str() + i + 1;
    }
  }
  token = token_c_str;
  if (!token.empty())
    tokens.push_back(token);

  // Purge dots
  for (auto& t : tokens) {
    if (t == ".")  // Skip single dots
      continue;

    if (t == "..") {
      if (final_tokens.empty()) {
        // Invalid path
        *path = "";
        return;
      }

      final_tokens.pop_back();
    } else {
      final_tokens.push_back(t);
    }
  }

  // Assemble final path
  *path = "file://";
  for (auto& t : final_tokens)
    *path += std::string("/") + t;
}

Status filelock_lock(const std::string& filename, int* fd, bool shared) {
  // Prepare the flock struct
  struct flock fl;
  if (shared)
    fl.l_type = F_RDLCK;
  else
    fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
  fl.l_pid = getpid();

  // Open the file
  *fd = ::open(filename.c_str(), O_RDWR);
  if (*fd == -1) {
    return LOG_STATUS(Status::StorageManagerError(
        std::string("Cannot open filelock '") + filename));
  }
  // Acquire the lock
  if (fcntl(*fd, F_SETLKW, &fl) == -1) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot lock consolidation filelock '") + filename));
  }
  return Status::Ok();
}

Status filelock_unlock(int fd) {
  if (::close(fd) == -1)
    return LOG_STATUS(Status::OSError(
        "Cannot unlock consolidation filelock: Cannot close filelock"));
  return Status::Ok();
}

Status move_dir(const std::string& old_path, const std::string& new_path) {
  if (rename(old_path.c_str(), new_path.c_str())) {
    return LOG_STATUS(
        Status::OSError(std::string("Cannot move path: ") + strerror(errno)));
  }
  return Status::Ok();
}

Status ls(const std::string& path, std::vector<std::string>* paths) {
  struct dirent* next_path;
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    return Status::Ok();
  }
  while ((next_path = readdir(dir))) {
    auto abspath = path + "/" + next_path->d_name;
    paths->push_back(abspath);
  }
  // close parent directory
  if (closedir(dir)) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot close parent directory; ") + strerror(errno)));
  }
  return Status::Ok();
}

Status create_file(const std::string& filename) {
  int fd = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_SYNC, S_IRWXU);
  if (fd == -1 || ::close(fd)) {
    return LOG_STATUS(Status::OSError(
        std::string("Failed to create file '") + filename + "'; " +
        strerror(errno)));
  }

  return Status::Ok();
}

Status read_from_file(
    const std::string& path, uint64_t offset, void* buffer, uint64_t length) {
  // Open file
  int fd = open(path.c_str(), O_RDONLY);
  if (fd == -1) {
    return LOG_STATUS(
        Status::OSError("Cannot read from file; File opening error"));
  }
  // Read
  lseek(fd, offset, SEEK_SET);
  int64_t bytes_read = ::read(fd, buffer, length);
  if (bytes_read != int64_t(length)) {
    return LOG_STATUS(
        Status::OSError("Cannot read from file; File reading error"));
  }
  // Close file
  if (close(fd)) {
    return LOG_STATUS(
        Status::OSError("Cannot read from file; File closing error"));
  }
  return Status::Ok();
}

Status read_from_file(const std::string& path, Buffer** buff) {
  std::ifstream file(path, std::ios::in | std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot read file '") + path + "': file open error"));
  }
  std::streampos nbytes = file.tellg();
  *buff = new Buffer(nbytes);
  file.seekg(0, std::ios::beg);
  file.read(static_cast<char*>((*buff)->data()), nbytes);
  file.close();
  return Status::Ok();
}

bool both_slashes(char a, char b) {
  return a == '/' && b == '/';
}

void adjacent_slashes_dedup(std::string* value) {
  assert(utils::starts_with(*value, "file://"));

  value->erase(
      std::unique(
          value->begin() + std::string("file://").size(),
          value->end(),
          both_slashes),
      value->end());
}

std::string abs_path(const std::string& path) {
  // Initialize current, home and root
  std::string current = current_dir();
  auto env_home_ptr = getenv("HOME");
  std::string home = env_home_ptr ? env_home_ptr : current;
  std::string root = "/";
  std::string posix_prefix = "file://";

  // Easy cases
  if (path == "" || path == "." || path == "./")
    return posix_prefix + current;
  if (path == "~")
    return posix_prefix + home;
  if (path == "/")
    return posix_prefix + root;

  // Other cases
  std::string ret_dir;
  if (utils::starts_with(path, posix_prefix))
    ret_dir = path;
  else if (utils::starts_with(path, "/"))
    ret_dir = posix_prefix + path;
  else if (utils::starts_with(path, "~/"))
    ret_dir = posix_prefix + home + path.substr(1, path.size() - 1);
  else if (utils::starts_with(path, "./"))
    ret_dir = posix_prefix + current + path.substr(1, path.size() - 1);
  else
    ret_dir = posix_prefix + current + "/" + path;

  adjacent_slashes_dedup(&ret_dir);
  purge_dots_from_path(&ret_dir);

  return ret_dir;
}

Status sync(const std::string& path) {
  // Open file
  int fd = -1;
  if (posix::is_dir(path))  // DIRECTORY
    fd = open(path.c_str(), O_RDONLY, S_IRWXU);
  else if (posix::is_file(path))  // FILE
    fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, S_IRWXU);
  else
    return Status::Ok();  // If file does not exist, exit

  // Handle error
  if (fd == -1) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot sync file '") + path + "'; File opening error"));
  }

  // Sync
  if (fsync(fd)) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot sync file '") + path + "'; File syncing error"));
  }

  // Close file
  if (close(fd)) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot sync file '") + path + "'; File closing error"));
  }

  // Success
  return Status::Ok();
}

Status write_to_file(
    const std::string& path, const void* buffer, uint64_t buffer_size) {
  // Open file
  int fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, S_IRWXU);
  if (fd == -1) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot write to file '") + path +
        "'; File opening error"));
  }

  // Append data to the file in batches of constants::max_write_bytes
  // bytes at a time
  int64_t bytes_written;
  while (buffer_size > constants::max_write_bytes) {
    bytes_written = ::write(fd, buffer, constants::max_write_bytes);
    if (bytes_written != constants::max_write_bytes) {
      return LOG_STATUS(Status::OSError(
          std::string("Cannot write to file '") + path +
          "'; File writing error"));
    }
    buffer_size -= constants::max_write_bytes;
  }
  bytes_written = ::write(fd, buffer, buffer_size);
  if (bytes_written != int64_t(buffer_size)) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot write to file '") + path +
        "'; File writing error"));
  }

  // Close file
  if (close(fd)) {
    return LOG_STATUS(Status::OSError(
        std::string("Cannot write to file '") + path +
        "'; File closing error"));
  }

  // Success
  return Status::Ok();
}

}  // namespace posix

}  // namespace tiledb