// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/public/cpp/is_potentially_trustworthy.h"

#include "base/command_line.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/no_destructor.h"
#include "base/optional.h"
#include "base/sequence_checker.h"
#include "base/strings/pattern.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "build/build_config.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/base/url_util.h"
#include "services/network/public/cpp/network_switches.h"
#include "url/origin.h"
#include "url/scheme_host_port.h"
#include "url/url_canon.h"
#include "url/url_canon_ip.h"
#include "url/url_canon_stdstring.h"
#include "url/url_constants.h"
#include "url/url_util.h"

namespace network {

namespace {

#if DCHECK_IS_ON()
base::Optional<base::SequenceChecker>& GetSequenceCheckerForAllowlist() {
  static base::NoDestructor<base::Optional<base::SequenceChecker>> s_checker;
  return *s_checker;
}
#endif

#if DCHECK_IS_ON()
#define DCHECK_ALLOWLIST_USED_ON_VALID_SEQUENCE()                            \
  do {                                                                       \
    if (GetSequenceCheckerForAllowlist().has_value()) {                      \
      DCHECK(                                                                \
          GetSequenceCheckerForAllowlist().value().CalledOnValidSequence()); \
    }                                                                        \
  } while (false);
#else  // DCHECK_IS_ON()
#define DCHECK_ALLOWLIST_USED_ON_VALID_SEQUENCE() EAT_STREAM_PARAMETERS
#endif

// |g_should_reparse_secure_origin_allowlist_cmdline| is needed to allow tests
// to trigger reparsing of the cmdline switch.  This global should remain set to
// |false| in production code.  See also thread safety notes in
// GetSecureOriginAllowlist().
bool g_should_reparse_secure_origin_allowlist_cmdline = false;

// Given a hostname pattern with a wildcard such as "*.foo.com", returns
// true if |hostname_pattern| meets both of these conditions:
// 1.) A string matching |hostname_pattern| is a valid hostname.
// 2.) Wildcards only appear beyond the eTLD+1. "*.foo.com" is considered
//     valid but "*.com" is not.
bool IsValidWildcardPattern(const std::string& hostname_pattern) {
  // Replace wildcards with dummy values to check whether a matching origin is
  // valid.
  std::string wildcards_replaced;
  if (!base::ReplaceChars(hostname_pattern, "*", "a", &wildcards_replaced))
    return false;
  // Construct a SchemeHostPort with a dummy scheme and port to check that the
  // hostname is valid.
  url::SchemeHostPort scheme_host_port(
      GURL(base::StringPrintf("http://%s:80", wildcards_replaced.c_str())));
  if (scheme_host_port.IsInvalid())
    return false;

  // Check that wildcards only appear beyond the eTLD+1.
  size_t registry_length =
      net::registry_controlled_domains::PermissiveGetHostRegistryLength(
          hostname_pattern,
          net::registry_controlled_domains::INCLUDE_UNKNOWN_REGISTRIES,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  // std::string::npos should only be returned for empty inputs, which should be
  // filtered out by the IsInvalid() check above.
  CHECK(registry_length != std::string::npos);
  // If there is no registrar portion, the pattern is considered invalid.
  if (registry_length == 0)
    return false;
  // If there is no component before the registrar portion, or if the component
  // immediately preceding the registrar portion contains a wildcard, the
  // pattern is not considered valid.
  std::string host_before_registrar =
      hostname_pattern.substr(0, hostname_pattern.size() - registry_length);
  std::vector<std::string> components =
      base::SplitString(host_before_registrar, ".", base::KEEP_WHITESPACE,
                        base::SPLIT_WANT_NONEMPTY);
  if (components.size() == 0)
    return false;
  if (components.back().find("*") != std::string::npos)
    return false;
  return true;
}

// Canonicalizes each component of |hostname_pattern|, making no changes to
// wildcard components or components that fail canonicalization. For example,
// given a |hostname_pattern| of "TeSt.*.%46oo.com", the output will be
// "test.*.foo.com".
std::string CanonicalizePatternComponents(const std::string& hostname_pattern) {
  std::string canonical_host;  // Do not modify outside of canon_output.
  canonical_host.reserve(hostname_pattern.length());
  url::StdStringCanonOutput canon_output(&canonical_host);

  for (size_t current = 0; current < hostname_pattern.length(); current++) {
    size_t begin = current;

    // Advance to next "." or end.
    current = hostname_pattern.find('.', begin);
    if (current == std::string::npos)
      current = hostname_pattern.length();

    // Try to append the canonicalized version of this component.
    int current_len = base::checked_cast<int>(current - begin);
    if (hostname_pattern.substr(begin, current_len) == "*" ||
        !url::CanonicalizeHostSubstring(
            hostname_pattern.data(),
            url::Component(base::checked_cast<int>(begin), current_len),
            &canon_output)) {
      // Failed to canonicalize this component; append as-is.
      canon_output.Append(hostname_pattern.substr(begin, current_len).data(),
                          current_len);
    }

    if (current < hostname_pattern.length())
      canon_output.push_back('.');
  }
  canon_output.Complete();
  return canonical_host;
}

std::vector<std::string> ParseSecureOriginAllowlistFromCmdline() {
  // If kUnsafelyTreatInsecureOriginAsSecure option is given, then treat the
  // value as a comma-separated list of origins or origin patterns. Callers
  // that need to also check the kUnsafelyTreatInsecureOriginAsSecure pref
  // value must instead use ParseAllowlist directly (as there is no way for
  // CreateAllowlist() to access prefs). For renderer processes the pref and
  // the switch will match, but for non-renderer processes the switch may
  // not be set.
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  std::string origins_str = "";
  if (command_line.HasSwitch(switches::kUnsafelyTreatInsecureOriginAsSecure)) {
    origins_str = command_line.GetSwitchValueASCII(
        switches::kUnsafelyTreatInsecureOriginAsSecure);
  }
  return ParseSecureOriginAllowlist(origins_str);
}

}  // namespace

bool IsOriginPotentiallyTrustworthy(const url::Origin& origin) {
  // The code below is based on the specification at
  // https://www.w3.org/TR/powerful-features/#is-origin-trustworthy.

  // 1. If origin is an opaque origin, return "Not Trustworthy".
  if (origin.opaque())
    return false;

  // 2. Assert: origin is a tuple origin.
  DCHECK(!origin.opaque());

  // 3. If origin’s scheme is either "https" or "wss", return "Potentially
  //    Trustworthy".
  if (GURL::SchemeIsCryptographic(origin.scheme()))
    return true;

  // 4. If origin’s host component matches one of the CIDR notations 127.0.0.0/8
  //    or ::1/128 [RFC4632], return "Potentially Trustworthy".
  //
  // Diverging from the spec a bit here - in addition to the hostnames covered
  // by https://www.w3.org/TR/powerful-features/#is-origin-trustworthy, the code
  // below also considers "localhost" to be potentially secure.
  //
  // Cannot just pass |origin.host()| to |HostStringIsLocalhost|, because of the
  // need to also strip the brackets from things like "[::1]".
  if (net::IsLocalhost(origin.GetURL()))
    return true;

  // 5. If origin’s scheme component is file, return "Potentially Trustworthy".
  //
  // This is somewhat redundant with the GetLocalSchemes-based check below.
  if (origin.scheme() == url::kFileScheme)
    return true;

  // 6. If origin’s scheme component is one which the user agent considers to be
  //    authenticated, return "Potentially Trustworthy".
  //    Note: See §7.1 Packaged Applications for detail here.
  //
  // Note that this ignores some schemes that are considered trustworthy by
  // higher layers (e.g. see GetSchemesBypassingSecureContextCheck in //chrome).
  //
  // See also
  // - content::ContentClient::AddAdditionalSchemes and
  //   content::ContentClient::Schemes::local_schemes and
  //   content::ContentClient::Schemes::secure_schemes
  // - url::AddLocalScheme
  // - url::AddSecureScheme
  if (base::ContainsValue(url::GetSecureSchemes(), origin.scheme()) ||
      base::ContainsValue(url::GetLocalSchemes(), origin.scheme())) {
    return true;
  }

  // 7. If origin has been configured as a trustworthy origin, return
  //    "Potentially Trustworthy".
  //    Note: See §7.2 Development Environments for detail here.
  if (IsAllowlistedAsSecureOrigin(origin, GetSecureOriginAllowlist()))
    return true;

  // 8. Return "Not Trustworthy".
  return false;
}

bool IsUrlPotentiallyTrustworthy(const GURL& url) {
  // The code below is based on the specification at
  // https://www.w3.org/TR/powerful-features/#is-url-trustworthy.

  // 1. If url’s scheme is "data", return "Not Trustworthy".
  //    Note: This aligns the definition of a secure context with the de facto
  //    "data: URL as opaque origin" behavior that a majority of today’s
  //    browsers have agreed upon, rather than the de jure "data: URL inherits
  //    origin" behavior defined in HTML.
  if (url.SchemeIs(url::kDataScheme))
    return false;

  // 2. If url is "about:blank" or "about:srcdoc", return "Potentially
  //    Trustworthy".
  if (url.SchemeIs(url::kAboutScheme))
    return true;

  // 3. Return the result of executing §3.2 Is origin potentially trustworthy?
  //    on url’s origin.
  //    Note: The origin of blob: and filesystem: URLs is the origin of the
  //    context in which they were created. Therefore, blobs created in a
  //    trustworthy origin will themselves be potentially trustworthy.
  return IsOriginPotentiallyTrustworthy(url::Origin::Create(url));
}

const std::vector<std::string>& GetSecureOriginAllowlist() {
  // This function will initialize |s_allowlist| in a thread-safe way because of
  // the way how |static| works - invoking base::NoDestructor's constructor and
  // ParseSecureOriginAllowlistFromCmdline in a thread-safe way and only once.
  //
  // OTOH, if ResetSecureOriginAllowlistForTesting forces reinitialization, then
  // things are not thread-safe anymore.  The DCHECK_ALLOWLIST_... below is
  // trying to making sure that tests behave correctly.
  DCHECK_ALLOWLIST_USED_ON_VALID_SEQUENCE();
  static base::NoDestructor<std::vector<std::string>> s_allowlist(
      ParseSecureOriginAllowlistFromCmdline());

  // If unit tests set |g_should_reparse_secure_origin_allowlist_cmdline| then
  // reading |g_should_reparse_secure_origin_allowlist_cmdline| as well as
  // reinitializing |s_allowlist| are NOT THREAD SAFE.  This seems okay for unit
  // tests (+ correct usage is verified by DCHECK_ALLOWLIST... above).
  if (g_should_reparse_secure_origin_allowlist_cmdline) {
    g_should_reparse_secure_origin_allowlist_cmdline = false;
    *s_allowlist = ParseSecureOriginAllowlistFromCmdline();
  }

  return *s_allowlist;
}

void ResetSecureOriginAllowlistForTesting() {
#if DCHECK_IS_ON()
  DCHECK_ALLOWLIST_USED_ON_VALID_SEQUENCE();
  // Enforce sequence-affinity only *after* the first call to
  // ResetSecureOriginAllowlistForTesting.
  if (!GetSequenceCheckerForAllowlist().has_value()) {
    // Construct a new base::SequenceChecked emplacing it into base::Optional.
    GetSequenceCheckerForAllowlist().emplace();
  }
#endif

  g_should_reparse_secure_origin_allowlist_cmdline = true;
}

bool IsAllowlistedAsSecureOrigin(const url::Origin& origin,
                                 const std::vector<std::string>& allowlist) {
  if (base::ContainsValue(allowlist, origin.Serialize()))
    return true;

  for (const std::string& origin_or_pattern : allowlist) {
    if (base::MatchPattern(origin.host(), origin_or_pattern))
      return true;
  }

  return false;
}

std::vector<std::string> ParseSecureOriginAllowlist(
    const std::string& origins_str) {
  std::vector<std::string> origin_patterns;
  for (const std::string& origin_str : base::SplitString(
           origins_str, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL)) {
    if (origin_str.find("*") != std::string::npos) {
      if (IsValidWildcardPattern(origin_str)) {
        std::string canonicalized_pattern =
            CanonicalizePatternComponents(origin_str);
        if (!canonicalized_pattern.empty()) {
          origin_patterns.push_back(canonicalized_pattern);
          continue;
        }
      }
      LOG(ERROR) << "Allowlisted secure origin pattern " << origin_str
                 << " is not valid; ignoring.";
      continue;
    }

    // Drop .unique() origins, as they are unequal to any other origins.
    url::Origin origin(url::Origin::Create(GURL(origin_str)));
    if (!origin.opaque())
      origin_patterns.push_back(origin.Serialize());
  }

  UMA_HISTOGRAM_COUNTS_100("Security.TreatInsecureOriginAsSecure",
                           origin_patterns.size());

#if defined(OS_CHROMEOS)
  // For Crostini, we allow access to the default VM/container as a secure
  // origin via the hostname penguin.linux.test. We are required to use a
  // wildcard for the prefix because we do not know what the port number is.
  // https://chromium.googlesource.com/chromiumos/docs/+/master/containers_and_vms.md
  origin_patterns.push_back("*.linux.test");
#endif

  return origin_patterns;
}

}  // namespace network