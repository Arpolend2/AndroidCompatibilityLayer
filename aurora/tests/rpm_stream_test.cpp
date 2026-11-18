#include "../rpm_writer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const QDir root(QString::fromLocal8Bit(argv[1]));
    QFile source(root.filePath(QStringLiteral("source.bin")));
    const QByteArray contents(150000, 'x');
    if (!source.open(QIODevice::WriteOnly) || source.write(contents) != contents.size()) return 1;
    source.close();
    RpmWriter::Package package;
    package.name = "ru.megler.StreamTest";
    package.version = "1.0";
    package.summary = "Stream test";
    package.description = "Stream test";
    package.buildTime = 1700000000;
    RpmWriter::StreamFile binary;
    binary.path = "/usr/share/ru.megler.StreamTest/binary.dat";
    binary.sourcePath = source.fileName().toStdString();
    RpmWriter::StreamFile desktop;
    desktop.path = "/usr/share/applications/ru.megler.StreamTest.desktop";
    desktop.content = "hello";
    const std::string output = root.filePath(QStringLiteral("test.rpm")).toStdString();
    std::string error;
    if (!RpmWriter::writeStreaming(package, {binary, desktop}, output, &error)) return 1;
    QFile original(QString::fromStdString(output));
    if (!original.open(QIODevice::ReadOnly)) return 1;
    const QByteArray data = original.readAll();
    original.close();
    if (data.isEmpty() || RpmWriter::writeStreaming(package, {binary, binary}, output, &error) ||
        error.empty() || !original.open(QIODevice::ReadOnly) || original.readAll() != data) return 1;
    original.close();
    binary.sourcePath = root.filePath(QStringLiteral("missing")).toStdString();
    if (RpmWriter::writeStreaming(package, {binary}, output, &error) || error.empty()) return 1;
    return 0;
}
