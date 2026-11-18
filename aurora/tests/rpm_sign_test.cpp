#include "../rpm_signers.h"
#include "../rpm_test_keys.h"

#include <QFile>

#include <cstdio>
#include <string>

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly)) return 1;
    const QByteArray before = file.readAll();
    file.close();
    if (before.isEmpty()) return 1;

    std::string log;
    if (!RpmSigners::signWithLibrary(argv[1], kTestKeyPem, kTestCertPem, log)) {
        std::fprintf(stderr, "%s\n", log.c_str());
        return 1;
    }
    if (!file.open(QIODevice::ReadOnly) || file.size() <= before.size() ||
        file.read(before.size()) != before) return 1;
    return 0;
}
