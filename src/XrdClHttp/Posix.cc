/**
 * This file is part of XrdClHttp
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#include "Posix.hh"

#include "XrdCl/XrdClStatus.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
#include "XrdCl/XrdClURL.hh"

#include "davix/auth/davixx509cred.hpp"
#include "davix/auth/davixauth.hpp"

#include <string>

namespace {

std::vector<std::string> SplitString(const std::string& input,
                                     const std::string& delimiter) {
  size_t start = 0;
  size_t end = 0;
  size_t length = 0;

  auto result = std::vector<std::string>{};

  do {
    end = input.find(delimiter, start);

    if (end != std::string::npos)
      length = end - start;
    else
      length = input.length() - start;

    if (length) result.push_back(input.substr(start, length));

    start = end + delimiter.size();
  } while (end != std::string::npos);

  return result;
}

void SetTimeout(Davix::RequestParams& params, uint16_t timeout) {
/*
 * At NERSC archive portal, we get error when setOperationTimeout()
 *
  if (timeout != 0) {
    struct timespec ts = {timeout, 0};
    params.setOperationTimeout(&ts);
  }
*/

  struct timespec ts = {0, 0};
  ts.tv_sec = 30;
  params.setConnectionTimeout(&ts);

  params.setOperationRetryDelay(2);
}

XrdCl::XRootDStatus FillStatInfo(const struct stat& stats, XrdCl::StatInfo* stat_info) {
  std::ostringstream data;
  data << stats.st_dev << " " << stats.st_size << " " << stats.st_mode << " "
       << stats.st_mtime;

  if (!stat_info->ParseServerResponse(data.str().c_str())) {
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errDataError);
  }

  return XrdCl::XRootDStatus();
}

// return NULL if no X509 proxy is found 
//Davix::X509Credential* LoadX509UserCredential() {
//  std::string myX509proxyFile;
//  if (getenv("X509_USER_PROXY") != NULL)
//    myX509proxyFile = getenv("X509_USER_PROXY");
//  else
//    myX509proxyFile = "/tmp/x509up_u" + std::to_string(geteuid());
//  
//  struct stat myX509proxyStat;
//  Davix::X509Credential* myX509proxy = NULL;
//  if (stat(myX509proxyFile.c_str(), &myX509proxyStat) == 0) {
//    myX509proxy = new Davix::X509Credential();
//    myX509proxy->loadFromFilePEM(myX509proxyFile.c_str(), myX509proxyFile.c_str(), "", NULL);
//  }
//  return myX509proxy;
//}

// see auth/davixauth.hpp
int LoadX509UserCredentialCallBack(void *userdata, 
                                   const Davix::SessionInfo &info,
                                   Davix::X509Credential *cert,
                                   Davix::DavixError **err) {
  std::string myX509proxyFile;
  if (getenv("X509_USER_PROXY") != NULL)
    myX509proxyFile = getenv("X509_USER_PROXY");
  else
    myX509proxyFile = "/tmp/x509up_u" + std::to_string(geteuid());
  
  struct stat myX509proxyStat;
  if (stat(myX509proxyFile.c_str(), &myX509proxyStat) == 0)
    return cert->loadFromFilePEM(myX509proxyFile.c_str(), myX509proxyFile.c_str(), "", err);
  else
    return 1;
}

void SetX509(Davix::RequestParams& params) {
  params.setClientCertCallbackX509(&LoadX509UserCredentialCallBack, NULL);

  //Davix::X509Credential* myX509proxy = LoadX509UserCredential();
  //if (myX509proxy != NULL) {
  //  params.setClientCertX509(*myX509proxy);
  //  delete myX509proxy;
  //}

  if (getenv("X509_CERT_DIR") != NULL)
    params.addCertificateAuthorityPath(getenv("X509_CERT_DIR"));
  else
    params.addCertificateAuthorityPath("/etc/grid-security/certificates");      
}

}  // namespace

namespace Posix {

using namespace XrdCl;

std::pair<DAVIX_FD*, XRootDStatus> Open(Davix::DavPosix& davix_client,
                                        const std::string& url, int flags,
                                        uint16_t timeout) {
  Davix::RequestParams params;
  SetTimeout(params, timeout);
  SetX509(params);
  Davix::DavixError* err = nullptr;
  DAVIX_FD* fd = davix_client.open(&params, url, flags, &err);
  auto status = !fd ? XRootDStatus(stError, errInternal, err->getStatus(),
                                   err->getErrMsg())
                    : XRootDStatus();
  return std::make_pair(fd, status);
}

XRootDStatus Close(Davix::DavPosix& davix_client, DAVIX_FD* fd) {
  Davix::DavixError* err = nullptr;
  if (davix_client.close(fd, &err)) {
    auto errStatus =
        XRootDStatus(stError, errInternal, err->getStatus(), err->getErrMsg());
    delete err;
    return errStatus;
  }

  return XRootDStatus();
}

XRootDStatus MkDir(Davix::DavPosix& davix_client, const std::string& path,
                   XrdCl::MkDirFlags::Flags flags, XrdCl::Access::Mode /*mode*/,
                   uint16_t timeout) {
  Davix::RequestParams params;
  SetTimeout(params, timeout);
  SetX509(params);

  auto DoMkDir = [&davix_client, &params](const std::string& path) {
    Davix::DavixError* err = nullptr;
    if (davix_client.mkdir(&params, path, S_IRWXU, &err) &&
        (err->getStatus() != Davix::StatusCode::FileExist)) {
      auto errStatus = XRootDStatus(stError, errInternal, err->getStatus(),
                                    err->getErrMsg());
      delete err;
      return errStatus;
    } else {
      return XRootDStatus();
    }
  };

  auto url = XrdCl::URL(path);

  if (flags & XrdCl::MkDirFlags::MakePath) {
    // Also create intermediate directories

    auto dirs = SplitString(url.GetPath(), "/");

    std::string dirs_cumul;
    for (const auto& d : dirs) {
      dirs_cumul += "/" + d;
      url.SetPath(dirs_cumul);
      auto status = DoMkDir(url.GetLocation());
      if (status.IsError()) {
        return status;
      }
    }
  } else {
    // Only create final directory
    auto status = DoMkDir(url.GetURL());
    if (status.IsError()) {
      return status;
    }
  }

  return XRootDStatus();
}

XRootDStatus RmDir(Davix::DavPosix& davix_client, const std::string& path,
                   uint16_t timeout) {
  Davix::RequestParams params;
  SetTimeout(params, timeout);
  SetX509(params);

  Davix::DavixError* err = nullptr;
  if (davix_client.rmdir(&params, path, &err)) {
    auto errStatus =
        XRootDStatus(stError, errInternal, err->getStatus(), err->getErrMsg());
    delete err;
    return errStatus;
  }

  return XRootDStatus();
}

std::pair<XrdCl::DirectoryList*, XrdCl::XRootDStatus> DirList(
    Davix::DavPosix& davix_client, const std::string& path, bool details,
    bool /*recursive*/, uint16_t timeout) {
  Davix::RequestParams params;
  SetTimeout(params, timeout);
  SetX509(params);

  auto dir_list = new DirectoryList();

  Davix::DavixError* err = nullptr;

  auto dir_fd = davix_client.opendirpp(&params, path, &err);
  if (!dir_fd) {
    auto errStatus = XRootDStatus(stError, errInternal, err->getStatus(),
                                  err->getErrMsg());
    delete err;
    return std::make_pair(nullptr, errStatus);
  }

  struct stat info;
  while (auto entry = davix_client.readdirpp(dir_fd, &info, &err)) {
    if (err) {
      auto errStatus = XRootDStatus(stError, errInternal, err->getStatus(),
                                    err->getErrMsg());
      delete err;
      return std::make_pair(nullptr, errStatus);
    }

    StatInfo* stat_info = nullptr;
    if (details) {
      stat_info = new StatInfo();
      auto res = FillStatInfo(info, stat_info);
      if (res.IsError()) {
        delete entry;
        delete stat_info;
        return std::make_pair(nullptr, res);
      }
    }

    auto list_entry = new DirectoryList::ListEntry(path, entry->d_name, stat_info);
    dir_list->Add(list_entry);

    delete entry;
  }

  if (davix_client.closedirpp(dir_fd, &err)) {
    auto errStatus = XRootDStatus(stError, errInternal, err->getStatus(),
                                  err->getErrMsg());
    delete err;
    return std::make_pair(nullptr, errStatus);
  }

  return std::make_pair(dir_list, XRootDStatus());
}

XRootDStatus Rename(Davix::DavPosix& davix_client, const std::string& source,
                    const std::string& dest, uint16_t timeout) {
  Davix::RequestParams params;
  SetTimeout(params, timeout);
  SetX509(params);

  Davix::DavixError* err = nullptr;
  if (davix_client.rename(&params, source, dest, &err)) {
    auto errStatus =
        XRootDStatus(stError, errInternal, err->getStatus(), err->getErrMsg());
    delete err;
    return errStatus;
  }

  return XRootDStatus();
}

XRootDStatus Stat(Davix::DavPosix& davix_client, const std::string& url,
                  uint16_t timeout, StatInfo* stat_info) {
  Davix::RequestParams params;
  SetTimeout(params, timeout);
  SetX509(params);

  struct stat stats;
  Davix::DavixError* err = nullptr;
  if (davix_client.stat(&params, url, &stats, &err)) {
    auto errStatus =
        XRootDStatus(stError, errInternal, err->getStatus(), err->getErrMsg());
    delete err;
    return errStatus;
  }

  auto res = FillStatInfo(stats, stat_info);
  if (res.IsError()) {
    return res;
  }

  return XRootDStatus();
}

XRootDStatus Unlink(Davix::DavPosix& davix_client, const std::string& url,
                    uint16_t timeout) {
  Davix::RequestParams params;
  SetTimeout(params, timeout);
  SetX509(params);

  Davix::DavixError* err = nullptr;
  if (davix_client.unlink(&params, url, &err)) {
    auto errStatus =
        XRootDStatus(stError, errInternal, err->getStatus(), err->getErrMsg());
    delete err;
    return errStatus;
  }

  return XRootDStatus();
}

std::pair<int, XRootDStatus> _PRead(Davix::DavPosix& davix_client, DAVIX_FD* fd,
                                    void* buffer, uint32_t size,
                                    uint64_t offset, bool no_pread = false) {
  Davix::DavixError* err = nullptr;
  int num_bytes_read;
  if (no_pread) { // continue reading from the current offset position
    num_bytes_read = davix_client.read(fd, buffer, size, &err); 
  }
  else {
    num_bytes_read = davix_client.pread(fd, buffer, size, offset, &err);
  }
  if (num_bytes_read < 0) {
    auto errStatus =
        XRootDStatus(stError, errInternal, err->getStatus(), err->getErrMsg());
    delete err;
    return std::make_pair(num_bytes_read, errStatus);
  }

  return std::make_pair(num_bytes_read, XRootDStatus());
}

std::pair<int, XRootDStatus> Read(Davix::DavPosix& davix_client, DAVIX_FD* fd,
                                  void* buffer, uint32_t size) {
  return _PRead(davix_client, fd, buffer, size, 0, true);
}

std::pair<int, XRootDStatus> PRead(Davix::DavPosix& davix_client, DAVIX_FD* fd,
                                   void* buffer, uint32_t size, uint64_t offset) {
  return _PRead(davix_client, fd, buffer, size, offset, false);
}

std::pair<int, XrdCl::XRootDStatus> PReadVec(Davix::DavPosix& davix_client,
                                             DAVIX_FD* fd,
                                             const XrdCl::ChunkList& chunks,
                                             void* buffer) {
  const auto num_chunks = chunks.size();
  std::vector<Davix::DavIOVecInput> input_vector(num_chunks);
  std::vector<Davix::DavIOVecOuput> output_vector(num_chunks);

  for (size_t i = 0; i < num_chunks; ++i) {
    input_vector[i].diov_offset = chunks[i].offset;
    input_vector[i].diov_size = chunks[i].length;
    input_vector[i].diov_buffer = chunks[i].buffer;
  }

  Davix::DavixError* err = nullptr;
  int num_bytes_read = davix_client.preadVec(
      fd, input_vector.data(), output_vector.data(), num_chunks, &err);
  if (num_bytes_read < 0) {
    auto errStatus =
        XRootDStatus(stError, errInternal, err->getStatus(), err->getErrMsg());
    delete err;
    return std::make_pair(num_bytes_read, XRootDStatus(stError, errUnknown));
  }

  return std::make_pair(num_bytes_read, XRootDStatus());
}

std::pair<int, XrdCl::XRootDStatus> PWrite(Davix::DavPosix& davix_client,
                                           DAVIX_FD* fd, uint64_t offset,
                                           uint32_t size, const void* buffer,
                                           uint16_t timeout) {
  Davix::DavixError* err = nullptr;
  int new_offset = davix_client.lseek(fd, offset, SEEK_SET, &err);
  if (uint64_t(new_offset) != offset) {
    auto errStatus =
        XRootDStatus(stError, errInternal, err->getStatus(), err->getErrMsg());
    delete err;
    return std::make_pair(new_offset, errStatus);
  }
  int num_bytes_written = davix_client.write(fd, buffer, size, &err);
  if (num_bytes_written < 0) {
    auto errStatus =
        XRootDStatus(stError, errInternal, err->getStatus(), err->getErrMsg());
    delete err;
    return std::make_pair(num_bytes_written, errStatus);
  }

  return std::make_pair(num_bytes_written, XRootDStatus());
}

}  // namespace Posix
