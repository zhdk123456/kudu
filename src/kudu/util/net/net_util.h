// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
#ifndef KUDU_UTIL_NET_NET_UTIL_H
#define KUDU_UTIL_NET_NET_UTIL_H

#include <string>
#include <vector>

#include "kudu/util/status.h"

namespace kudu {

class Sockaddr;

// A container for a host:port pair.
class HostPort {
 public:
  HostPort();
  HostPort(const std::string& host, uint16_t port);
  explicit HostPort(const Sockaddr& addr);

  // Parse a "host:port" pair into this object.
  // If there is no port specified in the string, then 'default_port' is used.
  Status ParseString(const std::string& str, uint16_t default_port);

  // Resolve any addresses corresponding to this host:port pair.
  // Note that a host may resolve to more than one IP address.
  //
  // 'addresses' may be NULL, in which case this function simply checks that
  // the host/port pair can be resolved, without returning anything.
  Status ResolveAddresses(std::vector<Sockaddr>* addresses) const;

  std::string ToString() const;

  const std::string& host() const { return host_; }
  void set_host(const std::string& host) { host_ = host; }

  uint16_t port() const { return port_; }
  void set_port(uint16_t port) { port_ = port; }

  // Parse a comma separated list of "host:port" pairs into a vector
  // HostPort objects. If no port is specified for an entry in the
  // comma separated list, 'default_port' is used for that entry's
  // pair.
  static Status ParseStrings(
      const std::string& comma_sep_addrs, uint16_t default_port, std::vector<HostPort>* res);

  // Takes a vector of HostPort objects and returns a comma separated
  // string containing of "host:port" pairs. This method is the
  // "inverse" of ParseStrings().
  static std::string ToCommaSeparatedString(const std::vector<HostPort>& host_ports);

 private:
  std::string host_;
  uint16_t port_;
};

// Parse and resolve the given comma-separated list of addresses.
//
// The resulting addresses will be resolved, made unique, and added to
// the 'addresses' vector.
//
// Any elements which do not include a port will be assigned 'default_port'.
Status ParseAddressList(const std::string& addr_list,
                        uint16_t default_port,
                        std::vector<Sockaddr>* addresses);

// Return true if the given port is likely to need root privileges to bind to.
bool IsPrivilegedPort(uint16_t port);

// Return the local machine's hostname.
Status GetHostname(std::string* hostname);

// Return the local machine's FQDN.
Status GetFQDN(std::string* fqdn);

// Try to run 'lsof' to determine which process is preventing binding to
// the given 'addr'. If pids can be determined, outputs full 'ps' and 'pstree'
// output for that process.
//
// Output is issued to the log at WARNING level, or appended to 'log' if it
// is non-NULL (mostly useful for testing).
void TryRunLsof(const Sockaddr& addr, std::vector<std::string>* log = NULL);

} // namespace kudu
#endif