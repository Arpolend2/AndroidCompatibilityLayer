#ifndef RPM_SIGNERS_H
#define RPM_SIGNERS_H

#include <string>

namespace RpmSigners {

// Signs the RPM in place by calling the private PackageSignatures::FileWithCmses::sign
// of librpmsign-external.so.3. Appends diagnostics to log.
bool signWithLibrary(const std::string &path, const char *keyPem, const char *certPem, std::string &log);

}

#endif
