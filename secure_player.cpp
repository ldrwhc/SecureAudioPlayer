#include <QApplication>
#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QAction>
#include <QBuffer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCollator>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QIcon>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMap>
#include <QMediaPlayer>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QMultiHash>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QQueue>
#include <QRandomGenerator>
#include <QRadioButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTableWidget>
#include <QTextStream>
#include <QToolButton>
#include <QToolTip>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QXmlStreamReader>
#include <QVector>

#include <private/qzipreader_p.h>
#include <private/qzipwriter_p.h>
#include <cstring>
#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#endif

namespace {
static const QByteArray kPakMagic = QByteArray::fromRawData("BUSAUD1\0", 8);
static const QByteArray kSynthMagicV1 = QByteArray::fromRawData("BUSSYN1\0", 8);
static const QByteArray kSynthMagicV2 = QByteArray::fromRawData("BUSSYN2\0", 8);
static const QString kDefaultKey = QStringLiteral("BusAnnouncement@2026");

struct AudioData {
    QByteArray pcmData;
    QAudioFormat format;
    qint64 durationMs = 0;
    bool isValid() const { return !pcmData.isEmpty() && format.isValid(); }
};

struct WavHeader {
    char chunkId[4] = {'R', 'I', 'F', 'F'};
    qint32 chunkSize = 0;
    char format[4] = {'W', 'A', 'V', 'E'};
    char subchunk1Id[4] = {'f', 'm', 't', ' '};
    qint32 subchunk1Size = 16;
    qint16 audioFormat = 1;
    qint16 numChannels = 1;
    qint32 sampleRate = 44100;
    qint32 byteRate = 44100 * 2;
    qint16 blockAlign = 2;
    qint16 bitsPerSample = 16;
    char subchunk2Id[4] = {'d', 'a', 't', 'a'};
    qint32 subchunk2Size = 0;
};

struct SynthesisContext {
    AudioData lineAudioChn;
    AudioData lineAudioEng;
    AudioData currentStationChn;
    AudioData currentStationEng;
    AudioData nextStationChn;
    AudioData nextStationEng;
    AudioData terminalStationChn;
    AudioData terminalStationEng;
};

struct TemplateConfig {
    QString filePath;
    QString name;
    bool hasEnglish = false;
    QMap<QString, QString> resources;
    QMap<QString, QStringList> sequences;
};

struct RuntimeTemplate {
    QString name;
    bool hasEnglish = false;
    QMap<QString, AudioData> resources;
    QMap<QString, QStringList> sequences;
};

struct PackManifest {
    QString key = kDefaultKey;
    QStringList packages;
    QHash<QString, QHash<QString, QString>> aliasesByPackageLower; // package -> (alias file -> original file name)
    QHash<QString, QString> kindsByPackageLower;                   // package -> kind
};

struct PackageInfo {
    QString pakPath;
    QString packageFileNameLower;
    QString packageKindLower;
    bool isEnglishPack = false;
    bool isTemplatePack = false;
    bool isPromptPack = false;
};

struct SourceEntry {
    int packageIndex = -1;
    QString zipEntryPath;
    QString zipEntryPathLower;
    QString diskPath;
    QString encryptedBlobPath;
    QString baseFileName;
    QString stem;
    QString suffix;
    bool isEnglish = false;
    bool isTemplate = false;
    bool isPrompt = false;
};

struct TrackItem {
    QString title;
    QString encryptedAudioPath;
    int sourceIndex = -1; // preview/direct-play source, avoid pre-decoding to memory
};

QString formatMs(qint64 ms) {
    if (ms < 0) ms = 0;
    const qint64 sec = ms / 1000;
    return QStringLiteral("%1:%2").arg(sec / 60, 2, 10, QChar('0')).arg(sec % 60, 2, 10, QChar('0'));
}

QString sanitizePathPart(QString text) {
    static const QRegularExpression badRe(QStringLiteral(R"([\\/:*?"<>|])"));
    text.replace(badRe, QStringLiteral("_"));
    text.replace('\n', QChar('_'));
    text.replace('\r', QChar('_'));
    text = text.trimmed();
    if (text.isEmpty()) text = QStringLiteral("track");
    return text;
}

bool isAudioSuffix(const QString &suffix) {
    const QString s = suffix.toLower();
    return s == QLatin1String("mp3") || s == QLatin1String("wav") || s == QLatin1String("m4a") ||
           s == QLatin1String("ogg") || s == QLatin1String("flac") || s == QLatin1String("aac") ||
           s == QLatin1String("wma");
}

int extensionPriority(const QString &suffix) {
    const QString s = suffix.toLower();
    if (s == QLatin1String("mp3")) return 6;
    if (s == QLatin1String("wav")) return 5;
    if (s == QLatin1String("m4a")) return 4;
    if (s == QLatin1String("ogg")) return 3;
    if (s == QLatin1String("flac")) return 2;
    if (s == QLatin1String("aac")) return 1;
    return 0;
}

QByteArray xorStreamCipher(const QByteArray &input, const QByteArray &key, const QByteArray &nonce) {
    QByteArray output = input;
    quint32 counter = 0;
    int offset = 0;
    while (offset < output.size()) {
        QByteArray seed;
        seed.reserve(key.size() + nonce.size() + 4);
        seed.append(key);
        seed.append(nonce);
        char cb[4] = {
            static_cast<char>(counter & 0xFF),
            static_cast<char>((counter >> 8) & 0xFF),
            static_cast<char>((counter >> 16) & 0xFF),
            static_cast<char>((counter >> 24) & 0xFF)};
        seed.append(cb, 4);
        const QByteArray block = QCryptographicHash::hash(seed, QCryptographicHash::Sha256);
        const int n = qMin(block.size(), output.size() - offset);
        for (int i = 0; i < n; ++i) {
            output[offset + i] = static_cast<char>(static_cast<unsigned char>(output[offset + i]) ^
                                                   static_cast<unsigned char>(block[i]));
        }
        offset += n;
        ++counter;
    }
    return output;
}

bool decryptPak(const QString &pakPath, const QString &keyText, QByteArray *zipBytes, QString *error) {
    QFile file(pakPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot open pak: %1").arg(pakPath);
        return false;
    }
    const QByteArray all = file.readAll();
    if (all.size() < 24 || all.first(8) != kPakMagic) {
        if (error) *error = QStringLiteral("Pak format invalid: %1").arg(QFileInfo(pakPath).fileName());
        return false;
    }
    const QByteArray nonce = all.mid(8, 16);
    const QByteArray plain = xorStreamCipher(all.mid(24), keyText.toUtf8(), nonce);
    if (!(plain.startsWith("PK\x03\x04") || plain.startsWith("PK\x05\x06") || plain.startsWith("PK\x07\x08"))) {
        if (error) *error = QStringLiteral("Pak decrypt failed: %1").arg(QFileInfo(pakPath).fileName());
        return false;
    }
    *zipBytes = plain;
    return true;
}

bool decryptPakToZipFile(const QString &pakPath, const QString &keyText, const QString &zipPath, QString *error) {
    QFile in(pakPath);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot open pak: %1").arg(pakPath);
        return false;
    }
    if (in.size() < 24) {
        if (error) *error = QStringLiteral("Pak format invalid: %1").arg(QFileInfo(pakPath).fileName());
        return false;
    }

    const QByteArray magic = in.read(8);
    if (magic != kPakMagic) {
        if (error) *error = QStringLiteral("Pak format invalid: %1").arg(QFileInfo(pakPath).fileName());
        return false;
    }
    const QByteArray nonce = in.read(16);
    if (nonce.size() != 16) {
        if (error) *error = QStringLiteral("Pak nonce invalid: %1").arg(QFileInfo(pakPath).fileName());
        return false;
    }

    QDir().mkpath(QFileInfo(zipPath).path());
    QFile out(zipPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("Cannot create temp zip: %1").arg(zipPath);
        return false;
    }

    const QByteArray key = keyText.toUtf8();
    quint32 counter = 0;
    QByteArray block;
    int blockPos = 32;

    auto refillBlock = [&]() {
        QByteArray seed;
        seed.reserve(key.size() + nonce.size() + 4);
        seed.append(key);
        seed.append(nonce);
        QByteArray ctr(4, Qt::Uninitialized);
        ctr[0] = static_cast<char>(counter & 0xFF);
        ctr[1] = static_cast<char>((counter >> 8) & 0xFF);
        ctr[2] = static_cast<char>((counter >> 16) & 0xFF);
        ctr[3] = static_cast<char>((counter >> 24) & 0xFF);
        seed.append(ctr);
        block = QCryptographicHash::hash(seed, QCryptographicHash::Sha256);
        ++counter;
        blockPos = 0;
    };

    QByteArray signatureProbe;
    signatureProbe.reserve(8);
    while (!in.atEnd()) {
        const QByteArray cipherChunk = in.read(1024 * 1024);
        if (cipherChunk.isEmpty()) break;
        QByteArray plainChunk(cipherChunk.size(), Qt::Uninitialized);
        for (int i = 0; i < cipherChunk.size(); ++i) {
            if (blockPos >= block.size()) refillBlock();
            plainChunk[i] = static_cast<char>(static_cast<unsigned char>(cipherChunk[i]) ^
                                              static_cast<unsigned char>(block[blockPos++]));
        }
        if (signatureProbe.size() < 8) {
            const int need = 8 - signatureProbe.size();
            signatureProbe.append(plainChunk.left(need));
        }
        if (out.write(plainChunk) != plainChunk.size()) {
            if (error) *error = QStringLiteral("Temp zip write failed: %1").arg(zipPath);
            out.close();
            out.remove();
            return false;
        }
    }
    out.close();

    const bool looksLikeZip = signatureProbe.startsWith("PK\x03\x04") ||
                              signatureProbe.startsWith("PK\x05\x06") ||
                              signatureProbe.startsWith("PK\x07\x08");
    if (!looksLikeZip) {
        if (error) *error = QStringLiteral("Pak decrypt failed: %1").arg(QFileInfo(pakPath).fileName());
        QFile::remove(zipPath);
        return false;
    }
    return true;
}

QString routeIdFromLineFile(const QString &linePath) {
    QString routeId = QFileInfo(linePath).completeBaseName();
    routeId.remove(QRegularExpression(QStringLiteral("[^0-9a-zA-Z]")));
    return routeId;
}

bool parseFirstInteger(const QString &text, qint64 *number) {
    if (!number) return false;
    static const QRegularExpression numRe(QStringLiteral(R"(-?\d+)"));
    const QRegularExpressionMatch m = numRe.match(text);
    if (!m.hasMatch()) return false;
    bool ok = false;
    const qint64 value = m.captured(0).toLongLong(&ok);
    if (!ok) return false;
    *number = value;
    return true;
}

#ifdef Q_OS_WIN
bool withEndpointVolume(const std::function<bool(IAudioEndpointVolume*)> &fn) {
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(hrInit);
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE) return false;

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioEndpointVolume *endpoint = nullptr;
    bool ok = false;

    do {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
        if (FAILED(hr) || !enumerator) break;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || !device) break;
        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&endpoint));
        if (FAILED(hr) || !endpoint) break;
        ok = fn(endpoint);
    } while (false);

    if (endpoint) endpoint->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (needUninit) CoUninitialize();
    return ok;
}

bool readSystemVolumePercent(int *outPercent) {
    if (!outPercent) return false;
    return withEndpointVolume([outPercent](IAudioEndpointVolume *endpoint) {
        float scalar = 1.0f;
        if (FAILED(endpoint->GetMasterVolumeLevelScalar(&scalar))) return false;
        *outPercent = qBound(0, qRound(scalar * 100.0f), 100);
        return true;
    });
}

bool writeSystemVolumePercent(int percent) {
    const float scalar = static_cast<float>(qBound(0, percent, 100)) / 100.0f;
    return withEndpointVolume([scalar](IAudioEndpointVolume *endpoint) {
        return SUCCEEDED(endpoint->SetMasterVolumeLevelScalar(scalar, nullptr));
    });
}
#else
bool readSystemVolumePercent(int *outPercent) {
    Q_UNUSED(outPercent);
    return false;
}

bool writeSystemVolumePercent(int percent) {
    Q_UNUSED(percent);
    return false;
}
#endif

bool isStationFileMatch(const QString &stationName, const QString &baseName) {
    const QString cleanStation = stationName.trimmed();
    const QString cleanBase = baseName.trimmed();
    if (cleanStation.isEmpty() || cleanBase.isEmpty()) return false;
    const QRegularExpression re(
        QStringLiteral("^%1(?:#\\d*|\\d+|\\x{00B7}.+|\\(.+\\)|\\x{FF08}.+\\x{FF09})?$")
            .arg(QRegularExpression::escape(cleanStation)));
    return re.match(cleanBase).hasMatch();
}

QStringList readTxtStations(const QString &path) {
    QStringList stations;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return stations;
    QTextStream ts(&file);
    ts.setEncoding(QStringConverter::Utf8);
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (!line.isEmpty()) stations << line;
    }
    return stations;
}

QStringList readXlsxStations(const QString &path) {
    QStringList stationList;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return stationList;
    QZipReader zip(&file);
    if (zip.status() != QZipReader::NoError) return stationList;

    QStringList sharedStrings;
    const QByteArray shared = zip.fileData(QStringLiteral("xl/sharedStrings.xml"));
    if (!shared.isEmpty()) {
        QXmlStreamReader xml(shared);
        while (!xml.atEnd()) {
            if (xml.readNextStartElement() && xml.name() == u"t") sharedStrings << xml.readElementText();
        }
    }

    const QByteArray sheetData = zip.fileData(QStringLiteral("xl/worksheets/sheet1.xml"));
    if (sheetData.isEmpty()) return stationList;
    QXmlStreamReader xml(sheetData);
    bool isFirstColumn = false;
    bool isSharedString = false;
    bool isInlineString = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (xml.name() == u"c") {
                const QString ref = xml.attributes().value(QStringLiteral("r")).toString();
                isFirstColumn = ref.startsWith(QLatin1Char('A'));
                const QString cellType = xml.attributes().value(QStringLiteral("t")).toString();
                isSharedString = (cellType == QLatin1String("s"));
                isInlineString = (cellType == QLatin1String("inlineStr"));
            } else if (xml.name() == u"v" && isFirstColumn) {
                const QString value = xml.readElementText();
                QString station;
                if (isSharedString) {
                    bool ok = false;
                    const int idx = value.toInt(&ok);
                    if (ok && idx >= 0 && idx < sharedStrings.size()) station = sharedStrings[idx];
                } else {
                    station = value;
                }
                station = station.trimmed();
                if (!station.isEmpty()) stationList << station;
            } else if (xml.name() == u"t" && isFirstColumn && isInlineString) {
                const QString station = xml.readElementText().trimmed();
                if (!station.isEmpty()) stationList << station;
            }
        }
    }
    return stationList;
}

bool isHighQualityExportStem(const QString &stem) {
    const QString base = stem.trimmed();
    if (base.isEmpty()) return false;
    static const QRegularExpression pureNumberPattern(QStringLiteral("^\\d+$"));
    static const QRegularExpression suffixDigitsPattern(QStringLiteral("\\d+$"));
    if (base.contains(QLatin1Char('#'))) return false;
    if (pureNumberPattern.match(base).hasMatch()) return false;
    if (suffixDigitsPattern.match(base).hasMatch()) return false;
    return true;
}

QString xmlEscaped(QString text) {
    text.replace('&', QStringLiteral("&amp;"));
    text.replace('<', QStringLiteral("&lt;"));
    text.replace('>', QStringLiteral("&gt;"));
    text.replace('\"', QStringLiteral("&quot;"));
    text.replace('\'', QStringLiteral("&apos;"));
    return text;
}

bool writeSimpleStationsXlsx(const QString &path, const QStringList &stations, QString *error) {
    QHash<QString, int> sharedIndex;
    QStringList sharedStrings;
    QList<int> stationSharedIds;
    stationSharedIds.reserve(stations.size());
    for (const QString &raw : stations) {
        const QString s = raw.trimmed();
        if (s.isEmpty()) continue;
        if (!sharedIndex.contains(s)) {
            sharedIndex.insert(s, sharedStrings.size());
            sharedStrings.push_back(s);
        }
        stationSharedIds.push_back(sharedIndex.value(s));
    }

    QByteArray sharedXml;
    {
        QTextStream ts(&sharedXml, QIODevice::WriteOnly);
        ts.setEncoding(QStringConverter::Utf8);
        ts << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
        ts << "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"";
        ts << " count=\"" << stationSharedIds.size() << "\"";
        ts << " uniqueCount=\"" << sharedStrings.size() << "\">";
        for (const QString &s : sharedStrings) {
            ts << "<si><t>" << xmlEscaped(s) << "</t></si>";
        }
        ts << "</sst>";
    }

    QByteArray sheetXml;
    {
        QTextStream ts(&sheetXml, QIODevice::WriteOnly);
        ts.setEncoding(QStringConverter::Utf8);
        ts << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
        ts << "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"";
        ts << " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
        if (!stationSharedIds.isEmpty()) {
            ts << "<dimension ref=\"A1:A" << stationSharedIds.size() << "\"/>";
        } else {
            ts << "<dimension ref=\"A1\"/>";
        }
        ts << "<sheetData>";
        for (int i = 0; i < stationSharedIds.size(); ++i) {
            const int row = i + 1;
            ts << "<row r=\"" << row << "\"><c r=\"A" << row << "\" t=\"s\"><v>"
               << stationSharedIds[i] << "</v></c></row>";
        }
        ts << "</sheetData></worksheet>";
    }

    static const QByteArray kContentTypes =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "<Override PartName=\"/xl/sharedStrings.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" "
        "ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
        "<Override PartName=\"/docProps/app.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
        "</Types>";
    static const QByteArray kRels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
        "Target=\"xl/workbook.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" "
        "Target=\"docProps/core.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" "
        "Target=\"docProps/app.xml\"/>"
        "</Relationships>";
    static const QByteArray kWorkbook =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<sheets><sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
    static const QByteArray kWorkbookRels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
        "Target=\"worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
        "Target=\"styles.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" "
        "Target=\"sharedStrings.xml\"/>"
        "</Relationships>";
    static const QByteArray kStyles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<fonts count=\"1\"><font><sz val=\"11\"/><color theme=\"1\"/><name val=\"Calibri\"/><family val=\"2\"/><scheme val=\"minor\"/></font></fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
        "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
        "</styleSheet>";
    static const QByteArray kCoreProps =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
        "xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
        "<dc:creator>HangzhouBus</dc:creator><cp:lastModifiedBy>HangzhouBus</cp:lastModifiedBy></cp:coreProperties>";
    static const QByteArray kAppProps =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
        "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">"
        "<Application>HangzhouBusVoice</Application></Properties>";

    QDir().mkpath(QFileInfo(path).path());
    QZipWriter writer(path);
    writer.setCompressionPolicy(QZipWriter::AutoCompress);
    writer.addFile(QStringLiteral("[Content_Types].xml"), kContentTypes);
    writer.addFile(QStringLiteral("_rels/.rels"), kRels);
    writer.addFile(QStringLiteral("xl/workbook.xml"), kWorkbook);
    writer.addFile(QStringLiteral("xl/_rels/workbook.xml.rels"), kWorkbookRels);
    writer.addFile(QStringLiteral("xl/worksheets/sheet1.xml"), sheetXml);
    writer.addFile(QStringLiteral("xl/sharedStrings.xml"), sharedXml);
    writer.addFile(QStringLiteral("xl/styles.xml"), kStyles);
    writer.addFile(QStringLiteral("docProps/core.xml"), kCoreProps);
    writer.addFile(QStringLiteral("docProps/app.xml"), kAppProps);
    writer.close();
    if (writer.status() != QZipWriter::NoError) {
        if (error) *error = QStringLiteral("xlsx 保存失败：%1").arg(path);
        return false;
    }
    return true;
}

AudioData decodeAudioFile(const QString &filePath) {
    AudioData data;
    if (!QFile::exists(filePath)) return data;
    QAudioDecoder decoder;
    decoder.setSource(QUrl::fromLocalFile(filePath));
    QAudioFormat target;
    target.setSampleRate(44100);
    target.setChannelCount(1);
    target.setSampleFormat(QAudioFormat::Int16);
    decoder.setAudioFormat(target);

    QByteArray pcm;
    QEventLoop loop;
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, [&]() {
        const QAudioBuffer b = decoder.read();
        if (b.isValid()) pcm.append(static_cast<const char*>(b.constData<void>()), b.byteCount());
    });
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, &QEventLoop::quit);
    QObject::connect(&decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &loop, &QEventLoop::quit);
    decoder.start();
    loop.exec();

    data.format = decoder.audioFormat().isValid() ? decoder.audioFormat() : target;
    data.pcmData = pcm;
    if (data.format.isValid()) {
        const int bps = data.format.bytesPerFrame() * data.format.sampleRate();
        if (bps > 0) data.durationMs = (data.pcmData.size() * 1000LL) / bps;
    }
    return data;
}

AudioData decodeAudioBytes(const QByteArray &audioBytes, const QString &nameHint = QString()) {
    AudioData data;
    if (audioBytes.isEmpty()) return data;

    QBuffer input;
    input.setData(audioBytes);
    if (!input.open(QIODevice::ReadOnly)) return data;

    QAudioDecoder decoder;
    QAudioFormat target;
    target.setSampleRate(44100);
    target.setChannelCount(1);
    target.setSampleFormat(QAudioFormat::Int16);
    decoder.setAudioFormat(target);

    Q_UNUSED(nameHint);
    decoder.setSourceDevice(&input);

    QByteArray pcm;
    QEventLoop loop;
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, [&]() {
        const QAudioBuffer b = decoder.read();
        if (b.isValid()) pcm.append(static_cast<const char*>(b.constData<void>()), b.byteCount());
    });
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, &QEventLoop::quit);
    QObject::connect(&decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &loop, &QEventLoop::quit);
    decoder.start();
    loop.exec();

    data.format = decoder.audioFormat().isValid() ? decoder.audioFormat() : target;
    data.pcmData = pcm;
    if (data.format.isValid()) {
        const int bps = data.format.bytesPerFrame() * data.format.sampleRate();
        if (bps > 0) data.durationMs = (data.pcmData.size() * 1000LL) / bps;
    }
    return data;
}

AudioData combineAudio(const QList<AudioData> &audioList) {
    AudioData out;
    if (audioList.isEmpty()) return out;
    for (const AudioData &a : audioList) {
        if (a.isValid()) {
            out.format = a.format;
            break;
        }
    }
    if (!out.format.isValid()) {
        out.format.setSampleRate(44100);
        out.format.setChannelCount(1);
        out.format.setSampleFormat(QAudioFormat::Int16);
    }
    const int bps = out.format.bytesPerFrame() * out.format.sampleRate();
    for (const AudioData &a : audioList) {
        if (!a.isValid()) continue;
        out.pcmData.append(a.pcmData);
        out.durationMs += (a.durationMs > 0) ? a.durationMs : (bps > 0 ? (a.pcmData.size() * 1000LL) / bps : 0);
    }
    return out;
}

AudioData createSilentAudioMs(int durationMs, const QAudioFormat &formatHint = QAudioFormat()) {
    const int ms = qMax(1, durationMs);
    AudioData silent;
    if (formatHint.isValid() && formatHint.sampleRate() > 0 && formatHint.bytesPerFrame() > 0 &&
        formatHint.sampleFormat() == QAudioFormat::Int16) {
        silent.format = formatHint;
    } else {
        silent.format.setSampleRate(44100);
        silent.format.setChannelCount(1);
        silent.format.setSampleFormat(QAudioFormat::Int16);
    }
    const int frameBytes = qMax(1, silent.format.bytesPerFrame());
    const int sampleRate = qMax(8000, silent.format.sampleRate());
    qint64 targetBytes = (static_cast<qint64>(frameBytes) * sampleRate * ms) / 1000;
    if (targetBytes < frameBytes) targetBytes = frameBytes;
    const qint64 rem = targetBytes % frameBytes;
    if (rem != 0) targetBytes += (frameBytes - rem);
    silent.pcmData.fill('\0', static_cast<int>(targetBytes));
    const int bps = frameBytes * sampleRate;
    silent.durationMs = bps > 0 ? (silent.pcmData.size() * 1000LL) / bps : ms;
    return silent;
}

bool saveAudioToWav(const AudioData &audio, const QString &outputPath) {
    if (!audio.isValid()) return false;
    QFile out(outputPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    WavHeader h;
    h.numChannels = static_cast<qint16>(audio.format.channelCount());
    h.sampleRate = audio.format.sampleRate();
    h.bitsPerSample = static_cast<qint16>(audio.format.bytesPerSample() * 8);
    h.blockAlign = static_cast<qint16>(audio.format.bytesPerFrame());
    h.byteRate = h.sampleRate * h.blockAlign;
    h.subchunk2Size = audio.pcmData.size();
    h.chunkSize = 36 + h.subchunk2Size;
    out.write(reinterpret_cast<const char*>(&h), sizeof(WavHeader));
    out.write(audio.pcmData);
    return true;
}

QByteArray audioToWavBytes(const AudioData &audio) {
    if (!audio.isValid()) return QByteArray();
    WavHeader h;
    h.numChannels = static_cast<qint16>(audio.format.channelCount());
    h.sampleRate = audio.format.sampleRate();
    h.bitsPerSample = static_cast<qint16>(audio.format.bytesPerSample() * 8);
    h.blockAlign = static_cast<qint16>(audio.format.bytesPerFrame());
    h.byteRate = h.sampleRate * h.blockAlign;
    h.subchunk2Size = audio.pcmData.size();
    h.chunkSize = 36 + h.subchunk2Size;

    QByteArray bytes;
    bytes.resize(static_cast<int>(sizeof(WavHeader)) + audio.pcmData.size());
    std::memcpy(bytes.data(), &h, sizeof(WavHeader));
    std::memcpy(bytes.data() + sizeof(WavHeader), audio.pcmData.constData(), audio.pcmData.size());
    return bytes;
}

bool writeEncryptedBlob(const QByteArray &plainBytes, const QString &outputPath, const QString &keyText, QString *error) {
    if (plainBytes.isEmpty()) {
        if (error) *error = QStringLiteral("写入失败：待加密数据为空");
        return false;
    }
    QByteArray nonce(16, Qt::Uninitialized);
    for (int i = 0; i < nonce.size(); ++i) {
        nonce[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    }
    const QByteArray keySeed = QCryptographicHash::hash(
        keyText.toUtf8() + QByteArrayLiteral("|BUS-BLOB-V2|") + nonce, QCryptographicHash::Sha256);
    const QByteArray cipher = xorStreamCipher(plainBytes, keySeed, nonce);
    const QByteArray tag = QCryptographicHash::hash(
                               keySeed + QByteArrayLiteral("|TAG|") + nonce + cipher, QCryptographicHash::Sha256)
                               .left(16);

    QByteArray all;
    all.reserve(kSynthMagicV2.size() + nonce.size() + tag.size() + cipher.size());
    all.append(kSynthMagicV2);
    all.append(nonce);
    all.append(tag);
    all.append(cipher);

    QDir().mkpath(QFileInfo(outputPath).path());
    QFile out(outputPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法写入加密文件：%1").arg(outputPath);
        return false;
    }
    if (out.write(all) != all.size()) {
        if (error) *error = QStringLiteral("加密文件写入不完整：%1").arg(outputPath);
        return false;
    }
    return true;
}

bool readEncryptedBlob(const QString &encryptedPath, const QString &keyText, QByteArray *plainBytes, QString *error) {
    QFile file(encryptedPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取加密文件：%1").arg(encryptedPath);
        return false;
    }
    const QByteArray all = file.readAll();
    if (all.size() >= (kSynthMagicV2.size() + 32) && all.first(kSynthMagicV2.size()) == kSynthMagicV2) {
        const QByteArray nonce = all.mid(kSynthMagicV2.size(), 16);
        const QByteArray tag = all.mid(kSynthMagicV2.size() + 16, 16);
        const QByteArray cipher = all.mid(kSynthMagicV2.size() + 32);
        const QByteArray keySeed = QCryptographicHash::hash(
            keyText.toUtf8() + QByteArrayLiteral("|BUS-BLOB-V2|") + nonce, QCryptographicHash::Sha256);
        const QByteArray expectTag = QCryptographicHash::hash(
                                         keySeed + QByteArrayLiteral("|TAG|") + nonce + cipher, QCryptographicHash::Sha256)
                                         .left(16);
        if (expectTag != tag) {
            if (error) *error = QStringLiteral("加密音频校验失败：%1").arg(QFileInfo(encryptedPath).fileName());
            return false;
        }
        const QByteArray plain = xorStreamCipher(cipher, keySeed, nonce);
        if (plain.isEmpty()) {
            if (error) *error = QStringLiteral("解密后音频为空：%1").arg(QFileInfo(encryptedPath).fileName());
            return false;
        }
        *plainBytes = plain;
        return true;
    }

    // 兼容旧格式 BUSSYN1
    if (all.size() >= (kSynthMagicV1.size() + 16) && all.first(kSynthMagicV1.size()) == kSynthMagicV1) {
        const QByteArray nonce = all.mid(kSynthMagicV1.size(), 16);
        const QByteArray plain = xorStreamCipher(all.mid(kSynthMagicV1.size() + 16), keyText.toUtf8(), nonce);
        if (plain.isEmpty()) {
            if (error) *error = QStringLiteral("解密后音频为空：%1").arg(QFileInfo(encryptedPath).fileName());
            return false;
        }
        *plainBytes = plain;
        return true;
    }

    if (error) *error = QStringLiteral("加密音频格式错误：%1").arg(QFileInfo(encryptedPath).fileName());
    return false;
}
} // namespace

class ResourceMapTableWidget : public QTableWidget {
public:
    explicit ResourceMapTableWidget(QWidget *parent = nullptr) : QTableWidget(parent) {
        setAcceptDrops(true);
        setDragEnabled(true);
        setDropIndicatorShown(true);
        setDragDropOverwriteMode(false);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
    }

    std::function<void(const QString &)> onDropValue;
    std::function<void(int fromRow, int toRow)> onInternalMove;
    int dragRow() const { return m_dragRow; }

protected:
    void startDrag(Qt::DropActions supportedActions) override {
        m_dragRow = currentRow();
        QTableWidget::startDrag(supportedActions);
    }

    void mousePressEvent(QMouseEvent *event) override {
        m_dragRow = rowAt(event->position().toPoint().y());
        QTableWidget::mousePressEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent *event) override {
        if (qobject_cast<QListWidget*>(event->source()) || event->source() == this) {
            event->acceptProposedAction();
            return;
        }
        QTableWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override {
        if (qobject_cast<QListWidget*>(event->source()) || event->source() == this) {
            event->acceptProposedAction();
            return;
        }
        QTableWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override {
        if (event->source() == this) {
            const int from = (m_dragRow >= 0) ? m_dragRow : currentRow();
            int to = indexAt(event->position().toPoint()).row();
            if (to < 0) to = rowCount() - 1;
            if (to >= rowCount()) to = rowCount() - 1;
            if (onInternalMove) onInternalMove(from, to);
            event->acceptProposedAction();
            m_dragRow = -1;
            return;
        }
        auto *srcList = qobject_cast<QListWidget*>(event->source());
        if (!srcList) {
            QTableWidget::dropEvent(event);
            return;
        }
        QString value;
        const QList<QListWidgetItem*> selected = srcList->selectedItems();
        if (!selected.isEmpty()) {
            value = selected.first()->data(Qt::UserRole + 1).toString().trimmed();
            if (value.isEmpty()) value = selected.first()->toolTip().trimmed();
            if (value.isEmpty()) value = selected.first()->text().trimmed();
        }
        if (value.isEmpty() && event->mimeData() && event->mimeData()->hasText()) {
            value = event->mimeData()->text().trimmed();
        }
        if (onDropValue) onDropValue(value);
        event->acceptProposedAction();
    }

private:
    int m_dragRow = -1;
};

class SecurePlayerWindow : public QWidget {
public:
    explicit SecurePlayerWindow(QWidget *parent = nullptr)
        : QWidget(parent), m_player(new QMediaPlayer(this)), m_audio(new QAudioOutput(this)) {
        setWindowTitle(QString::fromUtf8(u8"杭州公交报站语音库"));
        setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));
        resize(1200, 760);
        setStyleSheet(QStringLiteral(
            "QWidget { background: #1b1d22; color: #e8eaee; }"
            "QComboBox, QListWidget, QPushButton, QSlider { font-size: 15px; }"
            "QPushButton { background: #2b313b; border: 1px solid #4b5566; border-radius: 6px; padding: 8px 12px; }"
            "QPushButton:pressed { background: #3a4452; }"
            "QListWidget { background: #20242c; border: 1px solid #3a4452; }"
            "QComboBox { background: #252b34; border: 1px solid #3a4452; padding: 4px 8px; }"));
        m_player->setAudioOutput(m_audio);
        m_audio->setVolume(1.0);
        buildUi();
        connectSignals();
        initSystemVolumeSync();
        initRuntimeDirs();
        loadUserPreferences();
        reloadInputLists();
    }

    ~SecurePlayerWindow() override { cleanupSession(); }

protected:
    void closeEvent(QCloseEvent *event) override {
        saveUserPreferences();
        cleanupSession();
        QWidget::closeEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        placeLogClearButton();
    }

private:
    QComboBox *m_cfgCombo = nullptr;
    QComboBox *m_lineCombo = nullptr;
    QPushButton *m_reloadBtn = nullptr;
    QPushButton *m_loadBtn = nullptr;
    QPushButton *m_clearCacheBtn = nullptr;
    QMenuBar *m_topMenuBar = nullptr;
    QAction *m_previewAction = nullptr;
    QAction *m_addLineAction = nullptr;
    QAction *m_addConfigAction = nullptr;
    QAction *m_helpAction = nullptr;
    QAction *m_aboutAction = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_loadProgress = nullptr;
    QToolButton *m_clearLogBtn = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QListWidget *m_playlist = nullptr;
    QLabel *m_nowPlayingLabel = nullptr;
    QSlider *m_progress = nullptr;
    QLabel *m_timeLabel = nullptr;
    QToolButton *m_prevBtn = nullptr;
    QToolButton *m_playPauseBtn = nullptr;
    QToolButton *m_nextBtn = nullptr;
    QToolButton *m_modeBtn = nullptr;
    QToolButton *m_volumeBtn = nullptr;
    QSlider *m_volumeSlider = nullptr;
    QLabel *m_volumeValueLabel = nullptr;
    QWidget *m_volumePopup = nullptr;
    QVector<QPushButton*> m_promptButtons;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QTimer *m_volumeSyncTimer = nullptr;
    bool m_syncingSystemVolume = false;

    QString m_runtimeRoot;
    QString m_linesDir;
    QString m_configDir;
    QString m_packsDir;
    QString m_concatDir;
    QString m_concatEngDir;
    QString m_templateDir;
    QString m_templateLegacyDir;
    QString m_promptDir;
    QString m_sessionDir;
    QString m_packKey = kDefaultKey;
    QString m_sessionBlobKey;

    QVector<TemplateConfig> m_templates;
    QVector<PackageInfo> m_packages;
    QVector<SourceEntry> m_sources;
    QMultiHash<QString, int> m_indicesByLowerFileName;
    QMultiHash<QString, int> m_indicesByLowerStem;
    QHash<QString, int> m_bestResourceByLowerPath;
    QHash<QString, int> m_bestResourceByLowerName;

    QHash<QString, QByteArray> m_entryRawCache;
    QQueue<QString> m_entryRawOrder;
    qint64 m_entryRawCacheBytes = 0;
    QHash<QString, AudioData> m_entryAudioCache;
    QHash<QString, AudioData> m_stationAudioCache;
    QHash<QString, int> m_stationSourceIdxCache;
    QHash<QString, AudioData> m_resourceAudioCache;
    static constexpr qint64 kEntryRawCacheLimitBytes = 24 * 1024 * 1024; // 24MB

    int resourceSourceScore(const SourceEntry &s) const {
        int score = extensionPriority(s.suffix) * 100;
        if (!s.isEnglish) score += 30;
        if (s.isTemplate) score += 200;
        return score;
    }

    QVector<TrackItem> m_tracks;
    int m_currentTrack = -1;
    bool m_userSeeking = false;
    QByteArray m_livePlaybackBytes;
    QBuffer *m_livePlaybackDevice = nullptr;

    enum class PlaybackMode {
        SinglePlay,
        SingleLoop,
        Sequential,
        Random,
        ListLoop
    };
    PlaybackMode m_playMode = PlaybackMode::Sequential;

    struct SynthesisOptions {
        bool externalBroadcast = true;
        bool includeNextStation = true;
        bool highQuality = true;
        bool lowPass = false;
        bool blindMode = false;
        bool missingEngUseChinese = false;
    };
    SynthesisOptions m_synthOptions;
    QString m_savedCfgName;
    QString m_savedLineName;
    QString m_savedLinePath;

    void applySavedSelections() {
        if (m_cfgCombo && !m_savedCfgName.isEmpty()) {
            const int cfgIdx = m_cfgCombo->findText(m_savedCfgName, Qt::MatchExactly);
            if (cfgIdx >= 0) m_cfgCombo->setCurrentIndex(cfgIdx);
        }
        int lineIdx = -1;
        if (m_lineCombo && !m_savedLinePath.isEmpty()) {
            const QString target = QDir::cleanPath(m_savedLinePath).toLower();
            for (int i = 0; i < m_lineCombo->count(); ++i) {
                const QString dataPath = QDir::cleanPath(m_lineCombo->itemData(i).toString()).toLower();
                if (!dataPath.isEmpty() && dataPath == target) {
                    lineIdx = i;
                    break;
                }
            }
        }
        if (lineIdx < 0 && m_lineCombo && !m_savedLineName.isEmpty()) {
            lineIdx = m_lineCombo->findText(m_savedLineName, Qt::MatchExactly);
        }
        if (lineIdx >= 0 && m_lineCombo) m_lineCombo->setCurrentIndex(lineIdx);
    }

    void loadUserPreferences() {
        QSettings settings;
        settings.beginGroup(QStringLiteral("secure_player"));
        m_savedCfgName = settings.value(QStringLiteral("config_name"), m_savedCfgName).toString();
        m_savedLineName = settings.value(QStringLiteral("line_name"), m_savedLineName).toString();
        m_savedLinePath = settings.value(QStringLiteral("line_path"), m_savedLinePath).toString();
        m_synthOptions.externalBroadcast =
            settings.value(QStringLiteral("synth_external_broadcast"), m_synthOptions.externalBroadcast).toBool();
        m_synthOptions.includeNextStation =
            settings.value(QStringLiteral("synth_include_next_station"), m_synthOptions.includeNextStation).toBool();
        m_synthOptions.highQuality =
            settings.value(QStringLiteral("synth_high_quality"), m_synthOptions.highQuality).toBool();
        m_synthOptions.lowPass =
            settings.value(QStringLiteral("synth_low_pass"), m_synthOptions.lowPass).toBool();
        m_synthOptions.blindMode =
            settings.value(QStringLiteral("synth_blind_mode"), m_synthOptions.blindMode).toBool();
        m_synthOptions.missingEngUseChinese =
            settings.value(QStringLiteral("synth_missing_eng_use_chinese"), m_synthOptions.missingEngUseChinese).toBool();
        const int mode = settings.value(QStringLiteral("playback_mode"), static_cast<int>(m_playMode)).toInt();
        if (mode >= static_cast<int>(PlaybackMode::SinglePlay) &&
            mode <= static_cast<int>(PlaybackMode::ListLoop)) {
            m_playMode = static_cast<PlaybackMode>(mode);
        }
        settings.endGroup();
        updatePlaybackModeButton();
    }

    void saveUserPreferences() {
        if (m_cfgCombo && m_cfgCombo->currentIndex() >= 0) {
            m_savedCfgName = m_cfgCombo->currentText();
        }
        if (m_lineCombo && m_lineCombo->currentIndex() >= 0) {
            m_savedLineName = m_lineCombo->currentText();
            m_savedLinePath = m_lineCombo->currentData().toString();
        }
        QSettings settings;
        settings.beginGroup(QStringLiteral("secure_player"));
        settings.setValue(QStringLiteral("config_name"), m_savedCfgName);
        settings.setValue(QStringLiteral("line_name"), m_savedLineName);
        settings.setValue(QStringLiteral("line_path"), m_savedLinePath);
        settings.setValue(QStringLiteral("playback_mode"), static_cast<int>(m_playMode));
        settings.setValue(QStringLiteral("synth_external_broadcast"), m_synthOptions.externalBroadcast);
        settings.setValue(QStringLiteral("synth_include_next_station"), m_synthOptions.includeNextStation);
        settings.setValue(QStringLiteral("synth_high_quality"), m_synthOptions.highQuality);
        settings.setValue(QStringLiteral("synth_low_pass"), m_synthOptions.lowPass);
        settings.setValue(QStringLiteral("synth_blind_mode"), m_synthOptions.blindMode);
        settings.setValue(QStringLiteral("synth_missing_eng_use_chinese"), m_synthOptions.missingEngUseChinese);
        settings.endGroup();
        settings.sync();
    }

    void appendLog(const QString &text, bool isError = false) {
        const QString line = QStringLiteral("[%1] %2%3")
                                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")))
                                 .arg(isError ? QString::fromUtf8(u8"[错误] ") : QString())
                                 .arg(text);
        if (m_logView) m_logView->appendPlainText(line);
        qInfo().noquote() << line;
    }

    void updateLoadProgress(int percent, const QString &text, bool isError = false) {
        const int v = qBound(0, percent, 100);
        if (m_loadProgress) m_loadProgress->setValue(v);
        if (m_statusLabel) m_statusLabel->setText(text);
        appendLog(text, isError);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    void updatePlayPauseButton(bool isPlaying) {
        if (!m_playPauseBtn) return;
        m_playPauseBtn->setIcon(monoStyleIcon(isPlaying ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
        m_playPauseBtn->setToolTip(isPlaying ? QString::fromUtf8(u8"暂停") : QString::fromUtf8(u8"播放"));
    }

    void placeLogClearButton() {
        if (!m_logView || !m_clearLogBtn) return;
        const QSize buttonSize = m_clearLogBtn->sizeHint();
        const int margin = 8;
        m_clearLogBtn->move(m_logView->width() - buttonSize.width() - margin, margin);
        m_clearLogBtn->raise();
    }

    QString playbackModeText() const {
        switch (m_playMode) {
        case PlaybackMode::SinglePlay:
            return QString::fromUtf8(u8"单曲播放");
        case PlaybackMode::SingleLoop:
            return QString::fromUtf8(u8"单曲循环");
        case PlaybackMode::Random:
            return QString::fromUtf8(u8"随机播放");
        case PlaybackMode::ListLoop:
            return QString::fromUtf8(u8"列表循环");
        case PlaybackMode::Sequential:
        default:
            return QString::fromUtf8(u8"顺序播放");
        }
    }

    QIcon monoStyleIcon(QStyle::StandardPixmap sp, const QSize &size = QSize(20, 20)) const {
        QPixmap base = style()->standardIcon(sp).pixmap(size);
        QImage img = base.toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < img.height(); ++y) {
            QRgb *row = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const int a = qAlpha(row[x]);
                row[x] = qRgba(232, 234, 238, a);
            }
        }
        return QIcon(QPixmap::fromImage(img));
    }

    QIcon modeGlyphIcon(const QString &glyph) const {
        QPixmap pix(24, 24);
        pix.fill(Qt::transparent);
        QPainter painter(&pix);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QFont f = font();
        f.setBold(true);
        f.setPointSize(11);
        painter.setFont(f);
        painter.setPen(QColor(QStringLiteral("#e8eaee")));
        painter.drawText(pix.rect(), Qt::AlignCenter, glyph);
        return QIcon(pix);
    }

    QIcon playbackModeIcon() const {
        switch (m_playMode) {
        case PlaybackMode::SinglePlay:
            return modeGlyphIcon(QString::fromUtf8(u8"1▶"));
        case PlaybackMode::SingleLoop:
            return modeGlyphIcon(QString::fromUtf8(u8"1↻"));
        case PlaybackMode::Random:
            return modeGlyphIcon(QString::fromUtf8(u8"⇄"));
        case PlaybackMode::ListLoop:
            return modeGlyphIcon(QString::fromUtf8(u8"↻"));
        case PlaybackMode::Sequential:
        default:
            return modeGlyphIcon(QString::fromUtf8(u8"→"));
        }
    }

    void updateVolumeButtonIcon(int value) {
        if (!m_volumeBtn) return;
        m_volumeBtn->setIcon(monoStyleIcon(value <= 0 ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    }

    void styleControlButton(QToolButton *btn) {
        if (!btn) return;
        btn->setCursor(Qt::PointingHandCursor);
        btn->setAutoRaise(false);
        btn->setFixedSize(42, 42);
        btn->setIconSize(QSize(20, 20));
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { background: #2b313b; border: 1px solid #4b5566; border-radius: 8px; }"
            "QToolButton:hover { background: #36404d; }"
            "QToolButton:pressed { background: #3a4452; }"));
    }

    void stylePreviewActionButton(QToolButton *btn) {
        if (!btn) return;
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        btn->setIconSize(QSize(16, 16));
        btn->setMinimumHeight(34);
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { background: #2b313b; border: 1px solid #4b5566; border-radius: 6px; padding: 6px 10px; }"
            "QToolButton:hover { background: #36404d; }"
            "QToolButton:pressed { background: #3a4452; }"
            "QToolButton::menu-indicator { subcontrol-origin: padding; subcontrol-position: right center; right: 6px; }"));
    }

    void updateVolumeValueUi(int value, bool showTip) {
        const int v = qBound(0, value, 100);
        if (m_volumeValueLabel) m_volumeValueLabel->setText(QStringLiteral("%1%").arg(v));
        updateVolumeButtonIcon(v);
        if (showTip && m_volumeSlider && m_volumePopup && m_volumePopup->isVisible()) {
            const QPoint tipPos = m_volumeSlider->mapToGlobal(QPoint(m_volumeSlider->width() + 10, m_volumeSlider->height() / 2));
            QToolTip::showText(tipPos, QStringLiteral("%1%").arg(v), m_volumeSlider);
        }
    }

    void toggleVolumePopup() {
        if (!m_volumeBtn || !m_volumePopup) return;
        if (m_volumePopup->isVisible()) {
            m_volumePopup->hide();
            return;
        }
        syncVolumeFromSystem();
        const QSize sz = m_volumePopup->sizeHint();
        QPoint pos = m_volumeBtn->mapToGlobal(QPoint((m_volumeBtn->width() - sz.width()) / 2, -sz.height() - 8));
        if (pos.y() < 8) pos = m_volumeBtn->mapToGlobal(QPoint((m_volumeBtn->width() - sz.width()) / 2, m_volumeBtn->height() + 8));
        m_volumePopup->move(pos);
        m_volumePopup->show();
        m_volumePopup->raise();
        m_volumeSlider->setFocus();
    }

    void updatePlaybackModeButton() {
        if (!m_modeBtn) return;
        m_modeBtn->setText(QString());
        m_modeBtn->setIcon(playbackModeIcon());
        m_modeBtn->setToolTip(QString::fromUtf8(u8"播放模式：%1（点击切换）").arg(playbackModeText()));
        m_modeBtn->setIconSize(QSize(20, 20));
    }

    void syncVolumeFromSystem() {
        int systemPercent = -1;
        if (!readSystemVolumePercent(&systemPercent)) return;
        systemPercent = qBound(0, systemPercent, 100);
        if (m_volumeSlider && m_volumeSlider->value() != systemPercent) {
            m_syncingSystemVolume = true;
            {
                QSignalBlocker blocker(m_volumeSlider);
                m_volumeSlider->setValue(systemPercent);
            }
            m_syncingSystemVolume = false;
        }
        m_audio->setVolume(static_cast<qreal>(systemPercent) / 100.0);
        updateVolumeValueUi(systemPercent, false);
    }

    void initSystemVolumeSync() {
        if (!m_volumeSlider) return;
        syncVolumeFromSystem();
        m_volumeSyncTimer = new QTimer(this);
        m_volumeSyncTimer->setInterval(350);
        connect(m_volumeSyncTimer, &QTimer::timeout, this, [this]() { syncVolumeFromSystem(); });
        m_volumeSyncTimer->start();
    }

    int randomTrackIndex(int avoid) const {
        if (m_tracks.isEmpty()) return -1;
        if (m_tracks.size() == 1) return 0;
        int idx = QRandomGenerator::global()->bounded(m_tracks.size());
        if (idx == avoid) idx = (idx + 1 + QRandomGenerator::global()->bounded(m_tracks.size() - 1)) % m_tracks.size();
        return idx;
    }

    void cyclePlaybackMode() {
        switch (m_playMode) {
        case PlaybackMode::Sequential:
            m_playMode = PlaybackMode::SinglePlay;
            break;
        case PlaybackMode::SinglePlay:
            m_playMode = PlaybackMode::SingleLoop;
            break;
        case PlaybackMode::SingleLoop:
            m_playMode = PlaybackMode::Random;
            break;
        case PlaybackMode::Random:
            m_playMode = PlaybackMode::ListLoop;
            break;
        case PlaybackMode::ListLoop:
        default:
            m_playMode = PlaybackMode::Sequential;
            break;
        }
        updatePlaybackModeButton();
        saveUserPreferences();
        appendLog(QString::fromUtf8(u8"播放模式切换为：%1").arg(playbackModeText()));
    }

    QString sourceTypeText(const SourceEntry &s) const {
        if (s.isPrompt) return QString::fromUtf8(u8"提示");
        if (s.isTemplate) return QString::fromUtf8(u8"模板");
        return s.isEnglish ? QString::fromUtf8(u8"英文") : QString::fromUtf8(u8"中文");
    }

    QString sourceDisplayName(const SourceEntry &s) const {
        return s.stem.isEmpty() ? QFileInfo(s.baseFileName).completeBaseName() : s.stem;
    }

    void clearEntryRawCache() {
        m_entryRawCache.clear();
        m_entryRawOrder.clear();
        m_entryRawCacheBytes = 0;
    }

    void cacheEntryRawBytes(const QString &key, const QByteArray &bytes) {
        if (key.isEmpty() || bytes.isEmpty()) return;
        if (bytes.size() > kEntryRawCacheLimitBytes) return;

        if (m_entryRawCache.contains(key)) {
            m_entryRawCacheBytes -= m_entryRawCache.value(key).size();
            m_entryRawOrder.removeAll(key);
        }
        m_entryRawCache.insert(key, bytes);
        m_entryRawOrder.enqueue(key);
        m_entryRawCacheBytes += bytes.size();

        while (m_entryRawCacheBytes > kEntryRawCacheLimitBytes && !m_entryRawOrder.isEmpty()) {
            const QString oldKey = m_entryRawOrder.dequeue();
            if (!m_entryRawCache.contains(oldKey)) continue;
            m_entryRawCacheBytes -= m_entryRawCache.value(oldKey).size();
            m_entryRawCache.remove(oldKey);
        }
    }

    void replacePlaylistWithSourceIndices(const QVector<int> &sourceIndices, const QString &reason) {
        stopAndDetachPlayer();
        m_tracks.clear();
        m_playlist->clear();
        m_tracks.reserve(sourceIndices.size());
        for (int idx : sourceIndices) {
            if (idx < 0 || idx >= m_sources.size()) continue;
            const SourceEntry &s = m_sources[idx];
            TrackItem t;
            t.sourceIndex = idx;
            t.title = QStringLiteral("[%1]%2")
                          .arg(s.isEnglish ? QString::fromUtf8(u8"英") : QString::fromUtf8(u8"中"))
                          .arg(sourceDisplayName(s));
            m_tracks.push_back(t);
            m_playlist->addItem(t.title);
        }
        if (!m_tracks.isEmpty()) {
            m_playlist->setCurrentRow(0);
            m_statusLabel->setText(QString::fromUtf8(u8"已载入%1：%2 条").arg(reason).arg(m_tracks.size()));
            appendLog(QString::fromUtf8(u8"播放列表替换为%1，共 %2 条").arg(reason).arg(m_tracks.size()));
        } else {
            m_statusLabel->setText(QString::fromUtf8(u8"播放列表为空"));
            appendLog(QString::fromUtf8(u8"播放列表替换失败：没有可用条目"), true);
        }
    }

    bool ensureSourceIndexReady(QString *error) {
        return ensureSessionAndSources(error);
    }

    void openPreviewDialog() {
        QString err;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ready = ensureSourceIndexReady(&err);
        QApplication::restoreOverrideCursor();
        if (!ready) {
            QMessageBox::warning(this, QString::fromUtf8(u8"预览"), err);
            appendLog(QString::fromUtf8(u8"预览加载失败：%1").arg(err), true);
            return;
        }

        auto isPreviewCandidate = [this](const SourceEntry &s) {
            // Preview only station source from 00concat / 00concatEng.
            if (!s.diskPath.isEmpty()) {
                const QString abs = QDir::cleanPath(s.diskPath).toLower();
                const QString zhRoot = QDir::cleanPath(m_concatDir).toLower();
                const QString enRoot = QDir::cleanPath(m_concatEngDir).toLower();
                return abs.startsWith(zhRoot + QLatin1Char('/')) || abs.startsWith(zhRoot + QLatin1Char('\\')) ||
                       abs == zhRoot || abs.startsWith(enRoot + QLatin1Char('/')) ||
                       abs.startsWith(enRoot + QLatin1Char('\\')) || abs == enRoot;
            }
            if (s.packageIndex < 0 || s.packageIndex >= m_packages.size()) return false;
            const PackageInfo &pkg = m_packages[s.packageIndex];
            if (pkg.packageKindLower == QLatin1String("concat") || pkg.packageKindLower == QLatin1String("concat_eng")) {
                return true;
            }
            const QString pkgName = pkg.packageFileNameLower;
            return pkgName.contains(QStringLiteral("concat")) && !pkgName.contains(QStringLiteral("template")) &&
                   !pkgName.contains(QStringLiteral("prompt")) && !pkgName.contains(QStringLiteral("tip"));
        };

        QVector<int> previewIndices;
        previewIndices.reserve(m_sources.size());
        for (int i = 0; i < m_sources.size(); ++i) {
            if (isPreviewCandidate(m_sources[i])) {
                previewIndices.push_back(i);
            }
        }

        QCollator collator(QLocale(QLocale::Chinese, QLocale::China));
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        collator.setNumericMode(true);
        std::sort(previewIndices.begin(), previewIndices.end(), [this, &collator](int a, int b) {
            const SourceEntry &sa = m_sources[a];
            const SourceEntry &sb = m_sources[b];
            const QString nameA = sourceDisplayName(sa);
            const QString nameB = sourceDisplayName(sb);
            const int nameCmp = collator.compare(nameA, nameB);
            if (nameCmp != 0) return nameCmp < 0;
            if (sa.isEnglish != sb.isEnglish) return !sa.isEnglish && sb.isEnglish; // Chinese first under same name
            return collator.compare(sa.baseFileName, sb.baseFileName) < 0;
        });

        QDialog dlg(this);
        dlg.setWindowTitle(QString::fromUtf8(u8"预览"));
        dlg.resize(860, 620);

        auto *root = new QVBoxLayout(&dlg);
        auto *top = new QHBoxLayout();
        auto *searchLabel = new QLabel(QString::fromUtf8(u8"搜索音频："), &dlg);
        auto *searchEdit = new QLineEdit(&dlg);
        searchEdit->setPlaceholderText(QString::fromUtf8(u8"输入关键词（站名）"));
        auto *filterLabel = new QLabel(QStringLiteral("筛选:"), &dlg);
        auto *allBtn = new QRadioButton(QStringLiteral("all"), &dlg);
        auto *zhBtn = new QRadioButton(QString::fromUtf8(u8"中"), &dlg);
        auto *enBtn = new QRadioButton(QString::fromUtf8(u8"英"), &dlg);
        auto *langGroup = new QButtonGroup(&dlg);
        langGroup->addButton(allBtn, 0);
        langGroup->addButton(zhBtn, 1);
        langGroup->addButton(enBtn, 2);
        allBtn->setChecked(true);
        top->addWidget(searchLabel);
        top->addWidget(searchEdit, 1);
        top->addWidget(filterLabel);
        top->addWidget(allBtn);
        top->addWidget(zhBtn);
        top->addWidget(enBtn);
        root->addLayout(top);

        auto *list = new QListWidget(&dlg);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        root->addWidget(list, 1);

        auto *tip = new QLabel(&dlg);
        tip->setStyleSheet(QStringLiteral("color:#9eb5d4;"));
        root->addWidget(tip);

        auto *btnRow = new QHBoxLayout();
        auto *playBtn = new QToolButton(&dlg);
        auto *stopBtn = new QToolButton(&dlg);
        auto *batchPlayBtn = new QToolButton(&dlg);
        playBtn->setText(QString::fromUtf8(u8"播放选中"));
        stopBtn->setText(QString::fromUtf8(u8"停止"));
        batchPlayBtn->setText(QString::fromUtf8(u8"一键播放"));
        playBtn->setIcon(monoStyleIcon(QStyle::SP_MediaPlay));
        stopBtn->setIcon(monoStyleIcon(QStyle::SP_MediaStop));
        batchPlayBtn->setIcon(monoStyleIcon(QStyle::SP_MediaSeekForward));
        stylePreviewActionButton(playBtn);
        stylePreviewActionButton(stopBtn);
        stylePreviewActionButton(batchPlayBtn);
        batchPlayBtn->setPopupMode(QToolButton::InstantPopup);
        auto *batchMenu = new QMenu(batchPlayBtn);
        QAction *actPlayAll = batchMenu->addAction(QString::fromUtf8(u8"播放全部"));
        QAction *actPlayZh = batchMenu->addAction(QString::fromUtf8(u8"播放全部中文"));
        QAction *actPlayEn = batchMenu->addAction(QString::fromUtf8(u8"播放全部英文"));
        QAction *actPlayHQ = batchMenu->addAction(QString::fromUtf8(u8"播放全部高音质"));
        batchPlayBtn->setMenu(batchMenu);
        auto *closeBtn = new QPushButton(QString::fromUtf8(u8"关闭"), &dlg);
        btnRow->addWidget(playBtn);
        btnRow->addWidget(stopBtn);
        btnRow->addWidget(batchPlayBtn);
        btnRow->addStretch();
        btnRow->addWidget(closeBtn);
        root->addLayout(btnRow);

        auto refreshList = [&]() {
            const QString keyword = searchEdit->text().trimmed();
            const QString k = keyword.toLower();
            const int langFilter = langGroup->checkedId(); // 0 all, 1 zh, 2 en
            list->clear();
            int matched = 0;
            for (int idx : std::as_const(previewIndices)) {
                const SourceEntry &s = m_sources[idx];
                if (langFilter == 1 && s.isEnglish) continue;
                if (langFilter == 2 && !s.isEnglish) continue;
                const QString displayName = sourceDisplayName(s);
                const QString hay =
                    (displayName + QLatin1Char(' ') + s.baseFileName + QLatin1Char(' ') + s.stem + QLatin1Char(' ') + s.zipEntryPath + QLatin1Char(' ') +
                     s.diskPath)
                        .toLower();
                if (!k.isEmpty() && !hay.contains(k)) continue;
                ++matched;
                const QString text = QStringLiteral("[%1]%2")
                                         .arg(s.isEnglish ? QString::fromUtf8(u8"英") : QString::fromUtf8(u8"中"))
                                         .arg(displayName);
                auto *item = new QListWidgetItem(text, list);
                item->setData(Qt::UserRole, idx);
                item->setToolTip(s.zipEntryPath.isEmpty() ? s.diskPath : s.zipEntryPath);
            }
            tip->setText(QString::fromUtf8(u8"总计 %1 条，命中 %2 条。列表阶段仅索引展示，点击后才按需解密/解码。")
                             .arg(previewIndices.size())
                             .arg(matched));
            if (list->count() > 0) list->setCurrentRow(0);
        };

        auto playSelected = [&]() {
            const auto *item = list->currentItem();
            if (!item) return;
            const int idx = item->data(Qt::UserRole).toInt();
            if (idx < 0 || idx >= m_sources.size()) return;
            const SourceEntry &s = m_sources[idx];
            QString playErr;
            const QByteArray bytes = sourceBytes(s, &playErr);
            if (bytes.isEmpty()) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"预览"),
                                     playErr.isEmpty() ? QString::fromUtf8(u8"读取音频失败") : playErr);
                return;
            }
            if (!playSourceBytes(bytes, QString::fromUtf8(u8"预览: %1").arg(s.baseFileName), s.baseFileName, &playErr, false)) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"预览"),
                                     playErr.isEmpty() ? QString::fromUtf8(u8"播放失败") : playErr);
                return;
            }
            appendLog(QString::fromUtf8(u8"预览播放：%1").arg(s.baseFileName));
        };

        auto buildBatchIndices = [&](int mode) {
            QVector<int> out;
            out.reserve(previewIndices.size());
            if (mode == 0 || mode == 1 || mode == 2) {
                for (int idx : std::as_const(previewIndices)) {
                    const SourceEntry &s = m_sources[idx];
                    if (mode == 1 && s.isEnglish) continue;
                    if (mode == 2 && !s.isEnglish) continue;
                    out.push_back(idx);
                }
                return out;
            }

            // mode == 3: align with main2.cpp "导出高音质" rule:
            // only Chinese mp3, no '#', no pure number, no trailing digits.
            for (int idx : std::as_const(previewIndices)) {
                const SourceEntry &s = m_sources[idx];
                if (s.isEnglish) continue;
                if (s.suffix != QLatin1String("mp3")) continue;
                const QString stem = QFileInfo(s.baseFileName).completeBaseName().trimmed();
                if (!isHighQualityExportStem(stem)) continue;
                out.push_back(idx);
            }
            return out;
        };

        auto triggerBatchPlay = [&](int mode, const QString &label) {
            const QVector<int> indices = buildBatchIndices(mode);
            if (indices.isEmpty()) {
                QMessageBox::information(&dlg, QString::fromUtf8(u8"一键播放"), QString::fromUtf8(u8"没有可播放条目"));
                return;
            }
            replacePlaylistWithSourceIndices(indices, label);
            dlg.accept();
        };

        connect(searchEdit, &QLineEdit::textChanged, &dlg, [refreshList]() { refreshList(); });
        connect(searchEdit, &QLineEdit::returnPressed, &dlg, [playSelected]() { playSelected(); });
        connect(allBtn, &QRadioButton::toggled, &dlg, [refreshList](bool) { refreshList(); });
        connect(zhBtn, &QRadioButton::toggled, &dlg, [refreshList](bool) { refreshList(); });
        connect(enBtn, &QRadioButton::toggled, &dlg, [refreshList](bool) { refreshList(); });
        connect(list, &QListWidget::itemDoubleClicked, &dlg, [playSelected](QListWidgetItem *) { playSelected(); });
        connect(playBtn, &QToolButton::clicked, &dlg, [playSelected]() { playSelected(); });
        connect(stopBtn, &QToolButton::clicked, &dlg, [this]() { stopAndDetachPlayer(); });
        connect(actPlayAll, &QAction::triggered, &dlg, [triggerBatchPlay]() { triggerBatchPlay(0, QString::fromUtf8(u8"全部音频")); });
        connect(actPlayZh, &QAction::triggered, &dlg, [triggerBatchPlay]() { triggerBatchPlay(1, QString::fromUtf8(u8"全部中文")); });
        connect(actPlayEn, &QAction::triggered, &dlg, [triggerBatchPlay]() { triggerBatchPlay(2, QString::fromUtf8(u8"全部英文")); });
        connect(actPlayHQ, &QAction::triggered, &dlg, [triggerBatchPlay]() { triggerBatchPlay(3, QString::fromUtf8(u8"全部高音质")); });
        connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

        refreshList();
        dlg.exec();
    }

    void openUsageHelpDialog() {
        QDialog dlg(this);
        dlg.setWindowTitle(QString::fromUtf8(u8"使用帮助"));
        dlg.resize(760, 560);
        auto *root = new QVBoxLayout(&dlg);
        auto *text = new QPlainTextEdit(&dlg);
        text->setReadOnly(true);
        text->setPlainText(QString::fromUtf8(
            u8"杭州公交报站语音库 - 使用说明\n\n"
            u8"1. 加载配置与线路\n"
            u8"点击“加载配置与线路”，先确认合成参数，再选择配置与线路进行即时合成。\n\n"
            u8"2. 播放控制\n"
            u8"左侧为合成结果列表，右侧可播放/暂停、上一条/下一条、切换播放模式。\n\n"
            u8"3. 预览音频\n"
            u8"顶部“浏览 -> 浏览全部...”可按需检索并试听打包音频。\n"
            u8"支持 all/中/英筛选，且仅在点击播放时才会解密该条音频。\n\n"
            u8"4. 删除缓存\n"
            u8"“删除缓存”会清理程序运行时生成的临时加密文件。\n\n"
            u8"5. 数据目录\n"
            u8"程序目录下的 lines/config 用于配置与线路；packs 中存放加密音频包。"));
        auto *closeBtn = new QPushButton(QString::fromUtf8(u8"关闭"), &dlg);
        root->addWidget(text, 1);
        root->addWidget(closeBtn, 0, Qt::AlignRight);
        connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        dlg.exec();
    }

    void openAboutDialog() {
        QDialog dlg(this);
        dlg.setWindowTitle(QString::fromUtf8(u8"关于"));
        dlg.resize(520, 220);
        auto *root = new QVBoxLayout(&dlg);
        auto *title = new QLabel(QString::fromUtf8(u8"杭州公交报站语音库"), &dlg);
        title->setStyleSheet(QStringLiteral("font-size:20px; font-weight:700;"));
        auto *desc = new QLabel(
            QString::fromUtf8(u8"本程序用于公交报站语音资源的打包、检索、合成与播放。\n"
                               u8"感谢使用，欢迎访问开发者主页。"),
            &dlg);
        desc->setWordWrap(true);
        auto *link = new QLabel(QStringLiteral("<a href=\"https://github.com/ldrwhc\">https://github.com/ldrwhc</a>"), &dlg);
        link->setOpenExternalLinks(true);
        auto *closeBtn = new QPushButton(QString::fromUtf8(u8"关闭"), &dlg);
        root->addWidget(title);
        root->addWidget(desc);
        root->addWidget(link);
        root->addStretch();
        root->addWidget(closeBtn, 0, Qt::AlignRight);
        connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        dlg.exec();
    }

    void openAddLineDialog() {
        QDialog dlg(this);
        dlg.setWindowTitle(QString::fromUtf8(u8"添加线路"));
        dlg.resize(980, 680);

        auto *root = new QVBoxLayout(&dlg);
        auto *top = new QHBoxLayout();
        auto *nameLabel = new QLabel(QString::fromUtf8(u8"线路文件名："), &dlg);
        auto *nameEdit = new QLineEdit(&dlg);
        nameEdit->setPlaceholderText(QString::fromUtf8(u8"例如：100路 或 100路.xlsx"));
        auto *createBtn = new QPushButton(QString::fromUtf8(u8"创建/读取"), &dlg);
        top->addWidget(nameLabel);
        top->addWidget(nameEdit, 1);
        top->addWidget(createBtn);
        root->addLayout(top);

        auto *pathLabel = new QLabel(QString::fromUtf8(u8"当前文件：未选择"), &dlg);
        pathLabel->setStyleSheet(QStringLiteral("color:#9eb5d4;"));
        root->addWidget(pathLabel);

        auto *center = new QHBoxLayout();
        auto *table = new QTableWidget(&dlg);
        table->setColumnCount(1);
        table->setHorizontalHeaderLabels(QStringList() << QString::fromUtf8(u8"站名"));
        table->horizontalHeader()->setStretchLastSection(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::DoubleClicked |
                               QAbstractItemView::EditKeyPressed |
                               QAbstractItemView::AnyKeyPressed);
        center->addWidget(table, 1);

        auto *side = new QVBoxLayout();
        auto *insertBtn = new QPushButton(QStringLiteral("+"), &dlg);
        auto *deleteBtn = new QPushButton(QStringLiteral("-"), &dlg);
        auto *saveBtn = new QPushButton(QString::fromUtf8(u8"保存"), &dlg);
        insertBtn->setMinimumHeight(40);
        deleteBtn->setMinimumHeight(40);
        saveBtn->setMinimumHeight(40);
        side->addWidget(insertBtn);
        side->addWidget(deleteBtn);
        side->addSpacing(20);
        side->addWidget(saveBtn);
        side->addStretch();
        center->addLayout(side);
        root->addLayout(center, 1);

        auto *bottomBtns = new QHBoxLayout();
        auto *helpBtn = new QPushButton(QString::fromUtf8(u8"使用说明"), &dlg);
        auto *sampleBtn = new QPushButton(QString::fromUtf8(u8"示例填充"), &dlg);
        auto *closeBtn = new QPushButton(QString::fromUtf8(u8"关闭"), &dlg);
        bottomBtns->addWidget(helpBtn);
        bottomBtns->addWidget(sampleBtn);
        bottomBtns->addStretch();
        bottomBtns->addWidget(closeBtn);
        root->addLayout(bottomBtns);

        QString currentXlsxPath;

        auto loadStationsToTable = [&](const QStringList &stations) {
            table->setRowCount(0);
            for (const QString &s : stations) {
                const int row = table->rowCount();
                table->insertRow(row);
                table->setItem(row, 0, new QTableWidgetItem(s));
            }
            if (table->rowCount() == 0) {
                table->insertRow(0);
                table->setItem(0, 0, new QTableWidgetItem(QString()));
            }
            table->setCurrentCell(0, 0);
        };

        connect(createBtn, &QPushButton::clicked, &dlg, [&, this]() {
            QString name = nameEdit->text().trimmed();
            if (name.isEmpty()) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加线路"), QString::fromUtf8(u8"请输入线路文件名"));
                return;
            }
            static const QRegularExpression badRe(QStringLiteral(R"([\\/:*?"<>|])"));
            name.replace(badRe, QStringLiteral("_"));
            if (!name.endsWith(QStringLiteral(".xlsx"), Qt::CaseInsensitive)) {
                name += QStringLiteral(".xlsx");
            }
            nameEdit->setText(name);
            currentXlsxPath = QDir(m_linesDir).absoluteFilePath(name);

            if (QFile::exists(currentXlsxPath)) {
                const QStringList stations = readXlsxStations(currentXlsxPath);
                loadStationsToTable(stations);
                pathLabel->setText(QString::fromUtf8(u8"当前文件：%1（已读取）").arg(currentXlsxPath));
                appendLog(QString::fromUtf8(u8"线路编辑器已读取：%1").arg(name));
            } else {
                QString err;
                if (!writeSimpleStationsXlsx(currentXlsxPath, QStringList(), &err)) {
                    QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加线路"),
                                         err.isEmpty() ? QString::fromUtf8(u8"创建 xlsx 失败") : err);
                    return;
                }
                loadStationsToTable(QStringList());
                pathLabel->setText(QString::fromUtf8(u8"当前文件：%1（新建）").arg(currentXlsxPath));
                appendLog(QString::fromUtf8(u8"线路编辑器已新建：%1").arg(name));
            }
        });

        connect(insertBtn, &QPushButton::clicked, &dlg, [&, this]() {
            int row = table->currentRow();
            if (row < 0) row = table->rowCount();
            table->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(QString()));
            table->setCurrentCell(row, 0);
            table->editItem(table->item(row, 0));
        });

        connect(deleteBtn, &QPushButton::clicked, &dlg, [&, this]() {
            const int row = table->currentRow();
            if (row < 0 || row >= table->rowCount()) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加线路"), QString::fromUtf8(u8"请先选中要删除的行"));
                return;
            }
            table->removeRow(row);
            if (table->rowCount() == 0) {
                table->insertRow(0);
                table->setItem(0, 0, new QTableWidgetItem(QString()));
            }
        });

        connect(saveBtn, &QPushButton::clicked, &dlg, [&, this]() {
            if (currentXlsxPath.isEmpty()) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加线路"), QString::fromUtf8(u8"请先创建或读取线路文件"));
                return;
            }
            QStringList stations;
            for (int r = 0; r < table->rowCount(); ++r) {
                auto *it = table->item(r, 0);
                const QString s = (it ? it->text() : QString()).trimmed();
                if (!s.isEmpty()) stations << s;
            }
            QString err;
            if (!writeSimpleStationsXlsx(currentXlsxPath, stations, &err)) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加线路"),
                                     err.isEmpty() ? QString::fromUtf8(u8"保存失败") : err);
                return;
            }
            m_savedLineName = QFileInfo(currentXlsxPath).fileName();
            m_savedLinePath = currentXlsxPath;
            saveUserPreferences();
            reloadInputLists();
            appendLog(QString::fromUtf8(u8"线路保存完成：%1（%2 站）")
                          .arg(QFileInfo(currentXlsxPath).fileName())
                          .arg(stations.size()));
            QMessageBox::information(&dlg, QString::fromUtf8(u8"添加线路"), QString::fromUtf8(u8"保存成功"));
        });

        connect(helpBtn, &QPushButton::clicked, &dlg, [&]() {
            QMessageBox::information(
                &dlg,
                QString::fromUtf8(u8"添加线路 - 使用说明"),
                QString::fromUtf8(
                    u8"操作步骤：\n"
                    u8"1. 在“线路文件名”输入要创建或编辑的文件名（可不带 .xlsx）。\n"
                    u8"2. 点击“创建/读取”：不存在则新建，存在则读取。\n"
                    u8"3. 右侧“+”会在当前选中行之前插入空行；“-”删除当前选中行。\n"
                    u8"4. 编辑完成后点击“保存”，写回到 ./lines 下对应 xlsx。\n\n"
                    u8"示例：\n"
                    u8"线路文件名：示例线路.xlsx\n"
                    u8"站点：起点站 / 中间站 / 终点站"));
        });

        connect(sampleBtn, &QPushButton::clicked, &dlg, [&, this]() {
            if (nameEdit->text().trimmed().isEmpty()) {
                nameEdit->setText(QString::fromUtf8(u8"示例线路.xlsx"));
            }
            table->setRowCount(0);
            const QStringList demoStations = {
                QString::fromUtf8(u8"起点站"),
                QString::fromUtf8(u8"中间站"),
                QString::fromUtf8(u8"终点站")
            };
            for (const QString &s : demoStations) {
                const int row = table->rowCount();
                table->insertRow(row);
                table->setItem(row, 0, new QTableWidgetItem(s));
            }
            table->setCurrentCell(0, 0);
            appendLog(QString::fromUtf8(u8"线路编辑器已填充示例数据（未保存）"));
        });

        connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        dlg.exec();
    }

    void openAddConfigDialog() {
        {
            QDialog dlg(this);
            dlg.setWindowTitle(QString::fromUtf8(u8"添加配置"));
            dlg.resize(1320, 820);

            auto *root = new QVBoxLayout(&dlg);
            auto *top = new QHBoxLayout();
            auto *fileLabel = new QLabel(QString::fromUtf8(u8"配置文件名："), &dlg);
            auto *fileEdit = new QLineEdit(&dlg);
            fileEdit->setPlaceholderText(QString::fromUtf8(u8"例如：自定义模板.json"));
            auto *loadBtn = new QPushButton(QString::fromUtf8(u8"创建/读取"), &dlg);
            auto *nameLabel = new QLabel(QString::fromUtf8(u8"显示名："), &dlg);
            auto *nameEdit = new QLineEdit(&dlg);
            auto *engCheck = new QCheckBox(QString::fromUtf8(u8"启用英文报站"), &dlg);
            top->addWidget(fileLabel);
            top->addWidget(fileEdit, 2);
            top->addWidget(loadBtn);
            top->addSpacing(16);
            top->addWidget(nameLabel);
            top->addWidget(nameEdit, 2);
            top->addWidget(engCheck);
            root->addLayout(top);

            auto *pathLabel = new QLabel(QString::fromUtf8(u8"当前配置：未选择"), &dlg);
            pathLabel->setStyleSheet(QStringLiteral("color:#9eb5d4;"));
            root->addWidget(pathLabel);

            auto *middle = new QHBoxLayout();
            auto *leftPanel = new QWidget(&dlg);
            auto *left = new QVBoxLayout(leftPanel);
            left->addWidget(new QLabel(QString::fromUtf8(u8"模板音频"), &dlg));
            auto *searchEdit = new QLineEdit(&dlg);
            searchEdit->setPlaceholderText(QString::fromUtf8(u8"搜索模板音频"));
            left->addWidget(searchEdit);
            auto *candList = new QListWidget(&dlg);
            candList->setSelectionMode(QAbstractItemView::SingleSelection);
            candList->setDragEnabled(true);
            candList->setDragDropMode(QAbstractItemView::DragOnly);
            left->addWidget(candList, 1);
            auto *leftBtnRow = new QHBoxLayout();
            auto *importBtn = new QPushButton(QString::fromUtf8(u8"导入模板音频"), &dlg);
            auto *addResBtn = new QPushButton(QString::fromUtf8(u8"加入排序区域"), &dlg);
            auto *previewBtn = new QPushButton(QString::fromUtf8(u8"试听"), &dlg);
            leftBtnRow->addWidget(importBtn);
            leftBtnRow->addWidget(addResBtn);
            leftBtnRow->addWidget(previewBtn);
            left->addLayout(leftBtnRow);
            middle->addWidget(leftPanel, 4);

            auto *centerPanel = new QWidget(&dlg);
            auto *center = new QVBoxLayout(centerPanel);
            center->addWidget(new QLabel(QString::fromUtf8(u8"排序区域"), &dlg));
            auto *resTable = new ResourceMapTableWidget(&dlg);
            resTable->setColumnCount(2);
            resTable->setHorizontalHeaderLabels(QStringList() << QString::fromUtf8(u8"资源键/常量") << QString::fromUtf8(u8"文件路径/值"));
            resTable->horizontalHeader()->setStretchLastSection(true);
            resTable->setSelectionBehavior(QAbstractItemView::SelectRows);
            resTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
            resTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
            center->addWidget(resTable, 5);
            auto *sortBtns = new QHBoxLayout();
            auto *moveSortUpBtn = new QPushButton(QString::fromUtf8(u8"上移"), &dlg);
            auto *moveSortDownBtn = new QPushButton(QString::fromUtf8(u8"下移"), &dlg);
            auto *removeResBtn = new QPushButton(QString::fromUtf8(u8"删除选中项"), &dlg);
            sortBtns->addWidget(moveSortUpBtn);
            sortBtns->addWidget(moveSortDownBtn);
            sortBtns->addWidget(removeResBtn);
            sortBtns->addStretch();
            center->addLayout(sortBtns);

            center->addWidget(new QLabel(QString::fromUtf8(u8"四类合成序列"), &dlg));
            auto *seqCtrlRow = new QHBoxLayout();
            auto *seqSelect = new QComboBox(&dlg);
            seqSelect->addItem(QString::fromUtf8(u8"起点站"), QStringLiteral("start_station"));
            seqSelect->addItem(QString::fromUtf8(u8"进站"), QStringLiteral("enter_station"));
            seqSelect->addItem(QString::fromUtf8(u8"下站"), QStringLiteral("next_station"));
            seqSelect->addItem(QString::fromUtf8(u8"终点站"), QStringLiteral("terminal_station"));
            auto *addResToSeqBtn = new QPushButton(QString::fromUtf8(u8"从排序区域写入当前序列"), &dlg);
            seqCtrlRow->addWidget(new QLabel(QString::fromUtf8(u8"当前类别:"), &dlg));
            seqCtrlRow->addWidget(seqSelect, 1);
            seqCtrlRow->addWidget(addResToSeqBtn);
            center->addLayout(seqCtrlRow);

            auto *seqTable = new QTableWidget(&dlg);
            seqTable->setColumnCount(2);
            seqTable->setHorizontalHeaderLabels(QStringList() << QString::fromUtf8(u8"序列名") << QString::fromUtf8(u8"内容"));
            seqTable->horizontalHeader()->setStretchLastSection(true);
            seqTable->setRowCount(4);
            const QStringList seqNames = {
                QStringLiteral("start_station"),
                QStringLiteral("enter_station"),
                QStringLiteral("next_station"),
                QStringLiteral("terminal_station")
            };
            for (int i = 0; i < seqNames.size(); ++i) {
                auto *nameItem = new QTableWidgetItem(seqNames[i]);
                nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
                seqTable->setItem(i, 0, nameItem);
                seqTable->setItem(i, 1, new QTableWidgetItem(QString()));
            }
            seqTable->verticalHeader()->setDefaultSectionSize(30);
            const int seqH = seqTable->horizontalHeader()->height() + seqTable->frameWidth() * 2 + 30 * seqNames.size() + 4;
            seqTable->setMinimumHeight(seqH);
            seqTable->setMaximumHeight(seqH + 2);
            center->addWidget(seqTable, 0);
            middle->addWidget(centerPanel, 6);

            auto *rightPanel = new QWidget(&dlg);
            auto *right = new QVBoxLayout(rightPanel);
            right->addWidget(new QLabel(QString::fromUtf8(u8"常量池"), &dlg));
            auto *constList = new QListWidget(&dlg);
            constList->setSelectionMode(QAbstractItemView::SingleSelection);
            constList->setDragEnabled(true);
            constList->setDragDropMode(QAbstractItemView::DragOnly);
            const QStringList constTokens = {
                QStringLiteral("$LINE_NAME"), QStringLiteral("$LINE"),
                QStringLiteral("$LINE_NAME_EN"), QStringLiteral("$LINE_EN"),
                QStringLiteral("$CURRENT_STATION"), QStringLiteral("$CURRENT_STATION_EN"),
                QStringLiteral("$NEXT_STATION"), QStringLiteral("$NEXT_STATION_EN"),
                QStringLiteral("$TERMINAL"), QStringLiteral("$TERMINAL_STATION"),
                QStringLiteral("$TERMINAL_EN")
            };
            constList->addItems(constTokens);
            auto *addConstBtn = new QPushButton(QString::fromUtf8(u8"加入排序区域"), &dlg);
            right->addWidget(constList, 1);
            right->addWidget(addConstBtn);
            auto *constDesc = new QPlainTextEdit(&dlg);
            constDesc->setReadOnly(true);
            constDesc->setMaximumHeight(220);
            constDesc->setPlainText(QString::fromUtf8(
                u8"$LINE_NAME / $LINE：线路名（中文）\n"
                u8"$LINE_NAME_EN / $LINE_EN：线路名（英文）\n"
                u8"$CURRENT_STATION：当前站（中文）\n"
                u8"$CURRENT_STATION_EN：当前站（英文）\n"
                u8"$NEXT_STATION：下一站（中文）\n"
                u8"$NEXT_STATION_EN：下一站（英文）\n"
                u8"$TERMINAL / $TERMINAL_STATION：终点站（中文）\n"
                u8"$TERMINAL_EN：终点站（英文）"));
            right->addWidget(constDesc);
            right->addStretch();
            middle->addWidget(rightPanel, 3);

            root->addLayout(middle, 1);

            auto *bottom = new QHBoxLayout();
            auto *helpBtn = new QPushButton(QString::fromUtf8(u8"使用说明"), &dlg);
            auto *sampleBtn = new QPushButton(QString::fromUtf8(u8"示例填充"), &dlg);
            auto *saveBtn = new QPushButton(QString::fromUtf8(u8"保存配置"), &dlg);
            auto *closeBtn = new QPushButton(QString::fromUtf8(u8"关闭"), &dlg);
            bottom->addWidget(helpBtn);
            bottom->addWidget(sampleBtn);
            bottom->addStretch();
            bottom->addWidget(saveBtn);
            bottom->addWidget(closeBtn);
            root->addLayout(bottom);

            QString currentConfigPath;
            bool sourceReadyTried = false;
            struct TemplatePick {
                QString display;
                QString valueForJson;
                int sourceIdx = -1;
                QString localPath;
            };
            QVector<TemplatePick> picks;
            QMap<QString, QString> autoResources;
            QHash<QString, QString> keyByValueLower;
            QSet<QString> mappedConstants;
            QStringList sortAreaOrder;
            QString activeSeqKey = seqSelect->currentData().toString();
            int resourceCounter = 1;
            static const QRegularExpression splitRe(QStringLiteral(R"([,，\s]+)"));

            auto rowForSeq = [&](const QString &seqName) -> int {
                for (int r = 0; r < seqTable->rowCount(); ++r) {
                    const QString n = seqTable->item(r, 0) ? seqTable->item(r, 0)->text().trimmed() : QString();
                    if (n == seqName) return r;
                }
                return -1;
            };
            auto appendTokenToSeq = [&](const QString &seqName, const QString &token) {
                const QString t = token.trimmed();
                const int row = rowForSeq(seqName);
                if (row < 0 || t.isEmpty()) return;
                const QString oldText = seqTable->item(row, 1) ? seqTable->item(row, 1)->text().trimmed() : QString();
                const QString next = oldText.isEmpty() ? t : (oldText + QStringLiteral(", ") + t);
                if (!seqTable->item(row, 1)) seqTable->setItem(row, 1, new QTableWidgetItem(next));
                else seqTable->item(row, 1)->setText(next);
            };
            auto writeSortAreaToSeq = [&](const QString &seqName) {
                const int row = rowForSeq(seqName);
                if (row < 0) return;
                QStringList tokens;
                for (const QString &t : std::as_const(sortAreaOrder)) {
                    const QString token = t.trimmed();
                    if (!token.isEmpty()) tokens << token;
                }
                seqTable->item(row, 1)->setText(tokens.join(QStringLiteral(", ")));
            };
            auto tokenDisplay = [&](const QString &token) {
                if (token.startsWith(QLatin1Char('$'))) return token;
                if (!autoResources.contains(token)) return token;
                return QFileInfo(autoResources.value(token)).completeBaseName();
            };
            auto ensureTokenInSortArea = [&](const QString &token) {
                const QString t = token.trimmed();
                if (t.isEmpty()) return;
                sortAreaOrder << t;
            };
            auto removeTokenAtSortAreaRow = [&](int row) {
                if (row < 0 || row >= sortAreaOrder.size()) return QString();
                return sortAreaOrder.takeAt(row);
            };
            std::function<void(const QString &)> loadSortAreaFromSeq;
            auto refreshResourcesTable = [&]() {
                resTable->setRowCount(0);
                for (const QString &token : std::as_const(sortAreaOrder)) {
                    const int row = resTable->rowCount();
                    resTable->insertRow(row);
                    auto *k = new QTableWidgetItem(token);
                    auto *v = new QTableWidgetItem(tokenDisplay(token));
                    if (autoResources.contains(token)) v->setToolTip(autoResources.value(token));
                    else if (mappedConstants.contains(token)) v->setToolTip(QString::fromUtf8(u8"常量"));
                    else v->setToolTip(QString::fromUtf8(u8"来自原序列"));
                    resTable->setItem(row, 0, k);
                    resTable->setItem(row, 1, v);
                }
            };
            loadSortAreaFromSeq = [&](const QString &seqName) {
                sortAreaOrder.clear();
                const int row = rowForSeq(seqName);
                if (row < 0) {
                    refreshResourcesTable();
                    return;
                }
                const QString raw = seqTable->item(row, 1) ? seqTable->item(row, 1)->text() : QString();
                const QStringList tokens = raw.split(splitRe, Qt::SkipEmptyParts);
                for (const QString &token : tokens) {
                    const QString t = token.trimmed();
                    if (!t.isEmpty()) sortAreaOrder << t;
                }
                refreshResourcesTable();
            };
            auto ensureResourceKey = [&](const QString &valueForJson) {
                const QString v = valueForJson.trimmed();
                if (v.isEmpty()) return QString();
                const QString lower = v.toLower();
                if (keyByValueLower.contains(lower)) return keyByValueLower.value(lower);
                QString key;
                while (true) {
                    key = QStringLiteral("res%1").arg(resourceCounter++);
                    if (!autoResources.contains(key)) break;
                }
                autoResources.insert(key, v);
                keyByValueLower.insert(lower, key);
                ensureTokenInSortArea(key);
                refreshResourcesTable();
                return key;
            };
            auto ensureConstantMapped = [&](const QString &token) {
                const QString t = token.trimmed();
                if (!t.startsWith(QLatin1Char('$'))) return;
                mappedConstants.insert(t);
                ensureTokenInSortArea(t);
                refreshResourcesTable();
            };

            auto refreshCandidates = [&]() {
                if (!sourceReadyTried) {
                    QString err;
                    ensureSourceIndexReady(&err);
                    sourceReadyTried = true;
                }
                picks.clear();
                const QDir localTplDir(m_templateDir);
                if (localTplDir.exists()) {
                    QDirIterator it(m_templateDir, QDir::Files, QDirIterator::Subdirectories);
                    while (it.hasNext()) {
                        const QString full = it.next();
                        if (!isAudioSuffix(QFileInfo(full).suffix().toLower())) continue;
                        TemplatePick p;
                        const QString rel = localTplDir.relativeFilePath(full).replace('\\', '/');
                        p.valueForJson = QStringLiteral("template/%1").arg(rel);
                        p.display = QFileInfo(rel).completeBaseName();
                        p.localPath = full;
                        picks.push_back(p);
                    }
                }
                QSet<QString> seenLower;
                for (const auto &p : std::as_const(picks)) seenLower.insert(p.valueForJson.toLower());
                for (int i = 0; i < m_sources.size(); ++i) {
                    const SourceEntry &s = m_sources[i];
                    if (!s.isTemplate || s.baseFileName.isEmpty()) continue;
                    if (seenLower.contains(s.baseFileName.toLower())) continue;
                    TemplatePick p;
                    p.valueForJson = s.baseFileName;
                    p.sourceIdx = i;
                    p.display = QFileInfo(s.baseFileName).completeBaseName();
                    picks.push_back(p);
                    seenLower.insert(s.baseFileName.toLower());
                }
                QCollator coll(QLocale(QLocale::Chinese, QLocale::China));
                coll.setCaseSensitivity(Qt::CaseInsensitive);
                coll.setNumericMode(true);
                std::sort(picks.begin(), picks.end(), [&coll](const TemplatePick &a, const TemplatePick &b) {
                    const QString ka = a.display.left(1) + a.display;
                    const QString kb = b.display.left(1) + b.display;
                    return coll.compare(ka, kb) < 0;
                });
                const QString key = searchEdit->text().trimmed().toLower();
                candList->clear();
                for (int i = 0; i < picks.size(); ++i) {
                    const TemplatePick &p = picks[i];
                    const QString hay = (p.display + QLatin1Char(' ') + p.valueForJson).toLower();
                    if (!key.isEmpty() && !hay.contains(key)) continue;
                    auto *it = new QListWidgetItem(p.display, candList);
                    it->setData(Qt::UserRole, i);
                    it->setData(Qt::UserRole + 1, p.valueForJson);
                    it->setToolTip(p.valueForJson);
                }
                if (candList->count() > 0) candList->setCurrentRow(0);
            };

            auto loadConfigFile = [&](const QString &cfgPath) {
                currentConfigPath = cfgPath;
                pathLabel->setText(QString::fromUtf8(u8"当前配置：%1").arg(cfgPath));
                autoResources.clear();
                keyByValueLower.clear();
                mappedConstants.clear();
                sortAreaOrder.clear();
                resourceCounter = 1;
                for (int i = 0; i < seqNames.size(); ++i) seqTable->item(i, 1)->setText(QString());
                nameEdit->clear();
                engCheck->setChecked(false);
                if (!QFile::exists(cfgPath)) {
                    nameEdit->setText(QFileInfo(cfgPath).completeBaseName());
                    loadSortAreaFromSeq(activeSeqKey);
                    return;
                }
                QFile f(cfgPath);
                if (!f.open(QIODevice::ReadOnly)) return;
                const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
                if (!doc.isObject()) return;
                const QJsonObject obj = doc.object();
                nameEdit->setText(obj.value(QStringLiteral("name")).toString(QFileInfo(cfgPath).completeBaseName()));
                engCheck->setChecked(obj.value(QStringLiteral("eng")).toBool(false));
                const QJsonObject resObj = obj.value(QStringLiteral("resources")).toObject();
                QRegularExpression re(QStringLiteral("^res(\\d+)$"));
                for (auto it = resObj.begin(); it != resObj.end(); ++it) {
                    if (!it.value().isString()) continue;
                    autoResources.insert(it.key(), it.value().toString());
                    keyByValueLower.insert(it.value().toString().toLower(), it.key());
                    const auto m = re.match(it.key());
                    if (m.hasMatch()) resourceCounter = qMax(resourceCounter, m.captured(1).toInt() + 1);
                }
                const QJsonObject seqObj = obj.value(QStringLiteral("sequences")).toObject();
                for (const QString &seqName : seqNames) {
                    const int row = rowForSeq(seqName);
                    if (row < 0) continue;
                    const QJsonArray arr = seqObj.value(seqName).toArray();
                    QStringList tokens;
                    for (const QJsonValue &v : arr) {
                        if (!v.isString()) continue;
                        const QString t = v.toString().trimmed();
                        if (t.isEmpty()) continue;
                        tokens << t;
                        if (t.startsWith(QLatin1Char('$'))) mappedConstants.insert(t);
                    }
                    seqTable->item(row, 1)->setText(tokens.join(QStringLiteral(", ")));
                }
                loadSortAreaFromSeq(activeSeqKey);
            };

            auto importTemplateAudio = [&]() -> bool {
                const QString src = QFileDialog::getOpenFileName(&dlg, QString::fromUtf8(u8"导入模板音频"), m_templateDir,
                                                                  QStringLiteral("Audio Files (*.mp3 *.wav *.m4a *.ogg *.flac *.aac *.wma)"));
                if (src.isEmpty()) return false;
                const QString customDir = QDir(m_templateDir).absoluteFilePath(QStringLiteral("custom"));
                QDir().mkpath(customDir);
                QFileInfo fi(src);
                QString target = QDir(customDir).absoluteFilePath(fi.fileName());
                int idx = 1;
                while (QFile::exists(target)) {
                    target = QDir(customDir).absoluteFilePath(QStringLiteral("%1_%2.%3")
                                                                  .arg(fi.completeBaseName())
                                                                  .arg(idx++)
                                                                  .arg(fi.suffix().isEmpty() ? QStringLiteral("mp3") : fi.suffix()));
                }
                if (!QFile::copy(src, target)) return false;
                refreshCandidates();
                return true;
            };

            connect(loadBtn, &QPushButton::clicked, &dlg, [&, this]() {
                QString fileName = fileEdit->text().trimmed();
                if (fileName.isEmpty()) return;
                fileName.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
                if (!fileName.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) fileName += QStringLiteral(".json");
                fileEdit->setText(fileName);
                loadConfigFile(QDir(m_configDir).absoluteFilePath(fileName));
                refreshCandidates();
            });
            connect(searchEdit, &QLineEdit::textChanged, &dlg, [refreshCandidates]() { refreshCandidates(); });
            connect(importBtn, &QPushButton::clicked, &dlg, [importTemplateAudio]() { importTemplateAudio(); });
            connect(seqSelect, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, [&](int) {
                const QString newKey = seqSelect->currentData().toString();
                if (newKey.isEmpty() || newKey == activeSeqKey) return;
                writeSortAreaToSeq(activeSeqKey);
                activeSeqKey = newKey;
                loadSortAreaFromSeq(activeSeqKey);
                if (sortAreaOrder.isEmpty()) {
                    QMessageBox::information(&dlg, QString::fromUtf8(u8"切换类别"),
                                             QString::fromUtf8(u8"已保存上一类别内容。当前类别暂无内容，排序区域已清空，可重新设置。"));
                } else {
                    QMessageBox::information(&dlg, QString::fromUtf8(u8"切换类别"),
                                             QString::fromUtf8(u8"已保存上一类别内容，并载入当前类别已有排序内容。"));
                }
            });
            connect(addResBtn, &QPushButton::clicked, &dlg, [&]() {
                const auto *cur = candList->currentItem();
                if (!cur) return;
                const int idx = cur->data(Qt::UserRole).toInt();
                if (idx < 0 || idx >= picks.size()) return;
                ensureResourceKey(picks[idx].valueForJson);
            });
            connect(addConstBtn, &QPushButton::clicked, &dlg, [&]() {
                const auto *cur = constList->currentItem();
                if (cur) ensureConstantMapped(cur->text());
            });
            connect(constList, &QListWidget::itemDoubleClicked, &dlg, [&](QListWidgetItem *item) {
                if (item) ensureConstantMapped(item->text());
            });
            resTable->onDropValue = [&](const QString &valueText) {
                const QString v = valueText.trimmed();
                if (v.isEmpty()) return;
                if (v.startsWith(QLatin1Char('$'))) {
                    ensureConstantMapped(v);
                    return;
                }
                for (const TemplatePick &p : std::as_const(picks)) {
                    if (p.valueForJson.compare(v, Qt::CaseInsensitive) == 0 || p.display.compare(v, Qt::CaseInsensitive) == 0) {
                        ensureResourceKey(p.valueForJson);
                        return;
                    }
                }
            };
            resTable->onInternalMove = [&](int fromRow, int toRow) {
                if (fromRow < 0 || toRow < 0 || fromRow >= sortAreaOrder.size() || toRow >= sortAreaOrder.size()) return;
                if (fromRow == toRow) return;
                const QString token = sortAreaOrder.takeAt(fromRow);
                if (fromRow < toRow) --toRow;
                if (toRow < 0) toRow = 0;
                if (toRow > sortAreaOrder.size()) toRow = sortAreaOrder.size();
                sortAreaOrder.insert(toRow, token);
                refreshResourcesTable(); // absorb to row boundaries and refresh view
                resTable->setCurrentCell(toRow, 0);
            };
            auto moveSortRow = [&](int delta) {
                const int row = resTable->currentRow();
                if (row < 0) return;
                const int target = row + delta;
                if (target < 0 || target >= sortAreaOrder.size()) return;
                sortAreaOrder.swapItemsAt(row, target);
                refreshResourcesTable();
                resTable->setCurrentCell(target, 0);
            };
            connect(moveSortUpBtn, &QPushButton::clicked, &dlg, [&, moveSortRow]() { moveSortRow(-1); });
            connect(moveSortDownBtn, &QPushButton::clicked, &dlg, [&, moveSortRow]() { moveSortRow(+1); });
            connect(removeResBtn, &QPushButton::clicked, &dlg, [&]() {
                QList<int> rows;
                if (resTable->selectionModel()) {
                    const QModelIndexList selected = resTable->selectionModel()->selectedRows();
                    for (const QModelIndex &idx : selected) rows << idx.row();
                }
                if (rows.isEmpty()) rows << resTable->currentRow();
                rows.erase(std::remove_if(rows.begin(), rows.end(), [](int r) { return r < 0; }), rows.end());
                std::sort(rows.begin(), rows.end());
                rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
                if (rows.isEmpty()) return;

                QStringList removedTokens;
                for (int i = rows.size() - 1; i >= 0; --i) {
                    const int r = rows[i];
                    const QString key = removeTokenAtSortAreaRow(r).trimmed();
                    if (!key.isEmpty()) removedTokens << key;
                }
                if (removedTokens.isEmpty()) return;

                for (const QString &key : removedTokens) {
                    if (autoResources.contains(key) && !sortAreaOrder.contains(key)) {
                        keyByValueLower.remove(autoResources.value(key).toLower());
                        autoResources.remove(key);
                    }
                    if (mappedConstants.contains(key) && !sortAreaOrder.contains(key)) {
                        mappedConstants.remove(key);
                    }
                }
                refreshResourcesTable();
            });
            connect(addResToSeqBtn, &QPushButton::clicked, &dlg, [&]() {
                writeSortAreaToSeq(seqSelect->currentData().toString());
            });
            connect(previewBtn, &QPushButton::clicked, &dlg, [&, this]() {
                const auto *cur = candList->currentItem();
                if (!cur) return;
                const int idx = cur->data(Qt::UserRole).toInt();
                if (idx < 0 || idx >= picks.size()) return;
                const TemplatePick &p = picks[idx];
                QString err;
                QByteArray bytes;
                QString hint = QFileInfo(p.valueForJson).fileName();
                if (p.sourceIdx >= 0 && p.sourceIdx < m_sources.size()) {
                    bytes = sourceBytes(m_sources[p.sourceIdx], &err);
                    hint = m_sources[p.sourceIdx].baseFileName;
                } else if (!p.localPath.isEmpty()) {
                    QFile f(p.localPath);
                    if (f.open(QIODevice::ReadOnly)) bytes = f.readAll();
                }
                if (bytes.isEmpty()) return;
                playSourceBytes(bytes, QString::fromUtf8(u8"模板试听: %1").arg(p.valueForJson), hint, &err, false);
            });
            connect(saveBtn, &QPushButton::clicked, &dlg, [&, this]() {
                if (currentConfigPath.isEmpty()) return;
                writeSortAreaToSeq(activeSeqKey);
                QJsonObject rootObj;
                const QString displayName = nameEdit->text().trimmed().isEmpty()
                                                ? QFileInfo(currentConfigPath).completeBaseName()
                                                : nameEdit->text().trimmed();
                rootObj.insert(QStringLiteral("name"), displayName);
                rootObj.insert(QStringLiteral("eng"), engCheck->isChecked());
                QSet<QString> usedResourceKeys;
                QJsonObject seqObj;
                for (int i = 0; i < seqTable->rowCount(); ++i) {
                    const QString seqName = seqTable->item(i, 0) ? seqTable->item(i, 0)->text().trimmed() : QString();
                    const QStringList tokens = (seqTable->item(i, 1) ? seqTable->item(i, 1)->text() : QString()).split(splitRe, Qt::SkipEmptyParts);
                    QJsonArray arr;
                    for (const QString &t : tokens) {
                        arr.push_back(t);
                        if (!t.startsWith(QLatin1Char('$'))) usedResourceKeys.insert(t);
                    }
                    if (!seqName.isEmpty()) seqObj.insert(seqName, arr);
                }
                QJsonObject resObj;
                for (auto it = autoResources.begin(); it != autoResources.end(); ++it) {
                    if (usedResourceKeys.contains(it.key())) resObj.insert(it.key(), it.value());
                }
                rootObj.insert(QStringLiteral("resources"), resObj);
                rootObj.insert(QStringLiteral("sequences"), seqObj);
                QDir().mkpath(m_configDir);
                QFile out(currentConfigPath);
                if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
                out.write(QJsonDocument(rootObj).toJson(QJsonDocument::Indented));
                out.close();
                m_savedCfgName = displayName;
                saveUserPreferences();
                reloadInputLists();
                QMessageBox::information(&dlg, QString::fromUtf8(u8"添加配置"), QString::fromUtf8(u8"配置保存成功"));
            });
            connect(helpBtn, &QPushButton::clicked, &dlg, [&]() {
                QMessageBox::information(&dlg, QString::fromUtf8(u8"添加配置 - 使用说明"),
                                         QString::fromUtf8(u8"布局：上中下；中间为左中右；中列为上/下。\n"
                                                           u8"左侧模板和右侧常量都可拖拽到“排序区域”。\n"
                                                           u8"排序区域支持拖拽重排、上移/下移和删除。\n"
                                                           u8"切换当前类别时会先保存当前排序内容，再清空排序区域。\n"
                                                           u8"点“从排序区域写入当前序列”会把排序区域全部内容按顺序写入当前序列。"));
            });
            connect(sampleBtn, &QPushButton::clicked, &dlg, [&]() {
                if (fileEdit->text().trimmed().isEmpty()) fileEdit->setText(QString::fromUtf8(u8"示例配置.json"));
                QString cfgName = fileEdit->text().trimmed();
                if (!cfgName.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) cfgName += QStringLiteral(".json");
                fileEdit->setText(cfgName);
                currentConfigPath = QDir(m_configDir).absoluteFilePath(cfgName);
                pathLabel->setText(QString::fromUtf8(u8"当前配置：%1（示例，未保存）").arg(currentConfigPath));
                nameEdit->setText(QString::fromUtf8(u8"示例模板"));
                engCheck->setChecked(false);
                autoResources.clear();
                keyByValueLower.clear();
                mappedConstants.clear();
                sortAreaOrder.clear();
                resourceCounter = 1;
                for (int i = 0; i < seqTable->rowCount(); ++i) seqTable->item(i, 1)->setText(QString());
                const QString k1 = ensureResourceKey(QString::fromUtf8(u8"开往.mp3"));
                const QString k2 = ensureResourceKey(QString::fromUtf8(u8"起点站.mp3"));
                const QString k3 = ensureResourceKey(QString::fromUtf8(u8"终点站.mp3"));
                appendTokenToSeq(QStringLiteral("start_station"), k2);
                appendTokenToSeq(QStringLiteral("start_station"), QStringLiteral("$CURRENT_STATION"));
                appendTokenToSeq(QStringLiteral("enter_station"), QStringLiteral("$CURRENT_STATION"));
                appendTokenToSeq(QStringLiteral("enter_station"), k1);
                appendTokenToSeq(QStringLiteral("enter_station"), QStringLiteral("$TERMINAL"));
                appendTokenToSeq(QStringLiteral("next_station"), QStringLiteral("$NEXT_STATION"));
                appendTokenToSeq(QStringLiteral("terminal_station"), k3);
                appendTokenToSeq(QStringLiteral("terminal_station"), QStringLiteral("$TERMINAL"));
                ensureConstantMapped(QStringLiteral("$CURRENT_STATION"));
                ensureConstantMapped(QStringLiteral("$NEXT_STATION"));
                ensureConstantMapped(QStringLiteral("$TERMINAL"));
                refreshResourcesTable();
            });
            connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

            refreshCandidates();
            dlg.exec();
            return;
        }

        QDialog dlg(this);
        dlg.setWindowTitle(QString::fromUtf8(u8"添加配置"));
        dlg.resize(1280, 800);

        auto *root = new QVBoxLayout(&dlg);
        auto *top = new QHBoxLayout();
        auto *fileLabel = new QLabel(QString::fromUtf8(u8"配置文件名："), &dlg);
        auto *fileEdit = new QLineEdit(&dlg);
        fileEdit->setPlaceholderText(QString::fromUtf8(u8"例如：自定义模板.json"));
        auto *loadBtn = new QPushButton(QString::fromUtf8(u8"创建/读取"), &dlg);
        auto *nameLabel = new QLabel(QString::fromUtf8(u8"显示名："), &dlg);
        auto *nameEdit = new QLineEdit(&dlg);
        auto *engCheck = new QCheckBox(QString::fromUtf8(u8"启用英文报站"), &dlg);
        top->addWidget(fileLabel);
        top->addWidget(fileEdit, 2);
        top->addWidget(loadBtn);
        top->addSpacing(16);
        top->addWidget(nameLabel);
        top->addWidget(nameEdit, 2);
        top->addWidget(engCheck);
        root->addLayout(top);

        auto *pathLabel = new QLabel(QString::fromUtf8(u8"当前配置：未选择"), &dlg);
        pathLabel->setStyleSheet(QStringLiteral("color:#9eb5d4;"));
        root->addWidget(pathLabel);

        auto *middle = new QHBoxLayout();
        auto *leftPanel = new QWidget(&dlg);
        auto *left = new QVBoxLayout(leftPanel);
        left->addWidget(new QLabel(QString::fromUtf8(u8"可选模板音频（本地 ./template + pak 模板）"), &dlg));
        auto *searchEdit = new QLineEdit(&dlg);
        searchEdit->setPlaceholderText(QString::fromUtf8(u8"搜索模板音频"));
        left->addWidget(searchEdit);
        auto *candList = new QListWidget(&dlg);
        candList->setSelectionMode(QAbstractItemView::SingleSelection);
        candList->setDragEnabled(true);
        candList->setDragDropMode(QAbstractItemView::DragOnly);
        left->addWidget(candList, 1);
        auto *leftBtnRow = new QHBoxLayout();
        auto *importBtn = new QPushButton(QString::fromUtf8(u8"导入模板音频"), &dlg);
        auto *addResBtn = new QPushButton(QString::fromUtf8(u8"加入资源"), &dlg);
        auto *previewBtn = new QPushButton(QString::fromUtf8(u8"试听"), &dlg);
        leftBtnRow->addWidget(importBtn);
        leftBtnRow->addWidget(addResBtn);
        leftBtnRow->addWidget(previewBtn);
        left->addLayout(leftBtnRow);
        middle->addWidget(leftPanel, 5);

        auto *rightPanel = new QWidget(&dlg);
        auto *right = new QVBoxLayout(rightPanel);
        right->addWidget(new QLabel(QString::fromUtf8(u8"资源映射（key -> 文件）"), &dlg));
        auto *resTable = new ResourceMapTableWidget(&dlg);
        resTable->setColumnCount(2);
        resTable->setHorizontalHeaderLabels(QStringList() << QString::fromUtf8(u8"资源键") << QString::fromUtf8(u8"文件路径/模板名"));
        resTable->horizontalHeader()->setStretchLastSection(true);
        resTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        resTable->setSelectionMode(QAbstractItemView::SingleSelection);
        resTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        right->addWidget(resTable, 3);
        auto *resBtnRow = new QHBoxLayout();
        auto *removeResBtn = new QPushButton(QString::fromUtf8(u8"删除选中资源"), &dlg);
        auto *saveCurrentTypeBtn = new QPushButton(QString::fromUtf8(u8"保存当前类型"), &dlg);
        resBtnRow->addWidget(removeResBtn);
        resBtnRow->addWidget(saveCurrentTypeBtn);
        resBtnRow->addStretch();
        right->addLayout(resBtnRow);

        right->addWidget(new QLabel(QString::fromUtf8(u8"候选队列（左右两侧都可加入）"), &dlg));
        auto *seqCtrlRow = new QHBoxLayout();
        auto *seqSelect = new QComboBox(&dlg);
        seqSelect->addItem(QString::fromUtf8(u8"起点站"), QStringLiteral("start_station"));
        seqSelect->addItem(QString::fromUtf8(u8"进站"), QStringLiteral("enter_station"));
        seqSelect->addItem(QString::fromUtf8(u8"下站"), QStringLiteral("next_station"));
        seqSelect->addItem(QString::fromUtf8(u8"终点站"), QStringLiteral("terminal_station"));
        auto *addResToSeqBtn = new QPushButton(QString::fromUtf8(u8"加入资源到队列"), &dlg);
        auto *addConstBtn = new QPushButton(QString::fromUtf8(u8"加入常量"), &dlg);
        seqCtrlRow->addWidget(new QLabel(QString::fromUtf8(u8"当前类别:"), &dlg));
        seqCtrlRow->addWidget(seqSelect, 1);
        seqCtrlRow->addWidget(addResToSeqBtn);
        seqCtrlRow->addWidget(addConstBtn);
        right->addLayout(seqCtrlRow);

        auto *queueList = new QListWidget(&dlg);
        queueList->setSelectionMode(QAbstractItemView::SingleSelection);
        queueList->setDragDropMode(QAbstractItemView::InternalMove);
        queueList->setDefaultDropAction(Qt::MoveAction);
        right->addWidget(queueList, 2);

        auto *queueBtnRow = new QHBoxLayout();
        auto *moveUpBtn = new QPushButton(QString::fromUtf8(u8"上移"), &dlg);
        auto *moveDownBtn = new QPushButton(QString::fromUtf8(u8"下移"), &dlg);
        auto *removeTokenBtn = new QPushButton(QString::fromUtf8(u8"删除选中项"), &dlg);
        auto *clearQueueBtn = new QPushButton(QString::fromUtf8(u8"清空当前队列"), &dlg);
        queueBtnRow->addWidget(moveUpBtn);
        queueBtnRow->addWidget(moveDownBtn);
        queueBtnRow->addWidget(removeTokenBtn);
        queueBtnRow->addWidget(clearQueueBtn);
        queueBtnRow->addStretch();
        right->addLayout(queueBtnRow);

        right->addWidget(new QLabel(QString::fromUtf8(u8"常量池（右侧列表）"), &dlg));
        auto *constList = new QListWidget(&dlg);
        constList->setSelectionMode(QAbstractItemView::SingleSelection);
        constList->addItems(QStringList{
            QStringLiteral("$LINE_NAME"), QStringLiteral("$LINE"),
            QStringLiteral("$LINE_NAME_EN"), QStringLiteral("$LINE_EN"),
            QStringLiteral("$CURRENT_STATION"), QStringLiteral("$CURRENT_STATION_EN"),
            QStringLiteral("$NEXT_STATION"), QStringLiteral("$NEXT_STATION_EN"),
            QStringLiteral("$TERMINAL"), QStringLiteral("$TERMINAL_STATION"),
            QStringLiteral("$TERMINAL_EN")
        });
        right->addWidget(constList, 1);

        auto *seqTable = new QTableWidget(&dlg);
        seqTable->setColumnCount(2);
        seqTable->setHorizontalHeaderLabels(QStringList() << QString::fromUtf8(u8"序列名") << QString::fromUtf8(u8"内容"));
        seqTable->horizontalHeader()->setStretchLastSection(true);
        seqTable->setRowCount(4);
        const QStringList seqNames = {
            QStringLiteral("start_station"),
            QStringLiteral("enter_station"),
            QStringLiteral("next_station"),
            QStringLiteral("terminal_station")
        };
        const QStringList seqDefaults = {
            QString(),
            QString(),
            QString(),
            QString()
        };
        for (int i = 0; i < seqNames.size(); ++i) {
            auto *nameItem = new QTableWidgetItem(seqNames[i]);
            nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
            seqTable->setItem(i, 0, nameItem);
            seqTable->setItem(i, 1, new QTableWidgetItem(seqDefaults[i]));
        }
        right->addWidget(seqTable, 2);
        auto *hint = new QLabel(
            QString::fromUtf8(u8"操作：左侧模板加入资源映射 -> 中间加入队列；右侧常量可直接加入队列。"
                               u8"队列可拖拽/上下移动，按类别保存后写入下方序列。"),
            &dlg);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color:#9eb5d4;"));
        right->addWidget(hint);
        middle->addWidget(rightPanel, 7);
        root->addLayout(middle, 1);

        auto *bottom = new QHBoxLayout();
        auto *helpBtn = new QPushButton(QString::fromUtf8(u8"使用说明"), &dlg);
        auto *sampleBtn = new QPushButton(QString::fromUtf8(u8"示例填充"), &dlg);
        auto *saveBtn = new QPushButton(QString::fromUtf8(u8"保存配置"), &dlg);
        auto *closeBtn = new QPushButton(QString::fromUtf8(u8"关闭"), &dlg);
        bottom->addWidget(helpBtn);
        bottom->addWidget(sampleBtn);
        bottom->addStretch();
        bottom->addWidget(saveBtn);
        bottom->addWidget(closeBtn);
        root->addLayout(bottom);

        QString currentConfigPath;
        bool sourceReadyTried = false;
        struct TemplatePick {
            QString display;
            QString valueForJson;
            int sourceIdx = -1;
            QString localPath;
        };
        QVector<TemplatePick> picks;
        QMap<QString, QString> autoResources;    // key -> value
        QHash<QString, QString> keyByValueLower; // value.lower -> key
        int resourceCounter = 1;

        auto displayFromValue = [](const QString &value) {
            QString v = value;
            if (v.startsWith(QStringLiteral("template/"))) {
                v = QFileInfo(v).fileName();
            }
            return QFileInfo(v).completeBaseName();
        };

        auto refreshResourcesTable = [&]() {
            resTable->setRowCount(0);
            for (auto it = autoResources.begin(); it != autoResources.end(); ++it) {
                const int row = resTable->rowCount();
                resTable->insertRow(row);
                resTable->setItem(row, 0, new QTableWidgetItem(it.key()));
                resTable->setItem(row, 1, new QTableWidgetItem(displayFromValue(it.value())));
                resTable->item(row, 1)->setToolTip(it.value());
            }
        };

        auto ensureResourceKey = [&](const QString &valueForJson) {
            const QString v = valueForJson.trimmed();
            if (v.isEmpty()) return QString();
            const QString lower = v.toLower();
            if (keyByValueLower.contains(lower)) return keyByValueLower.value(lower);
            QString key;
            while (true) {
                key = QStringLiteral("res%1").arg(resourceCounter++);
                if (!autoResources.contains(key)) break;
            }
            autoResources.insert(key, v);
            keyByValueLower.insert(lower, key);
            refreshResourcesTable();
            return key;
        };

        static const QRegularExpression splitRe(QStringLiteral(R"([,，\s]+)"));
        auto rowForSeq = [&](const QString &seqName) -> int {
            for (int r = 0; r < seqTable->rowCount(); ++r) {
                const QString n = seqTable->item(r, 0) ? seqTable->item(r, 0)->text().trimmed() : QString();
                if (n == seqName) return r;
            }
            return -1;
        };
        auto tokensFromQueue = [&]() {
            QStringList tokens;
            for (int i = 0; i < queueList->count(); ++i) {
                const QString t = queueList->item(i)->text().trimmed();
                if (!t.isEmpty()) tokens << t;
            }
            return tokens;
        };
        auto saveCurrentTypeToTable = [&]() {
            const QString seqName = seqSelect->currentData().toString();
            const int row = rowForSeq(seqName);
            if (row < 0) return;
            const QString joined = tokensFromQueue().join(QStringLiteral(", "));
            if (!seqTable->item(row, 1)) seqTable->setItem(row, 1, new QTableWidgetItem(joined));
            else seqTable->item(row, 1)->setText(joined);
        };
        auto loadCurrentTypeQueue = [&]() {
            const QString seqName = seqSelect->currentData().toString();
            const int row = rowForSeq(seqName);
            queueList->clear();
            if (row < 0) return;
            const QString raw = seqTable->item(row, 1) ? seqTable->item(row, 1)->text().trimmed() : QString();
            const QStringList tokens = raw.split(splitRe, Qt::SkipEmptyParts);
            for (const QString &t : tokens) queueList->addItem(t);
            if (queueList->count() > 0) queueList->setCurrentRow(0);
        };
        auto appendTokenToQueue = [&](const QString &token) {
            const QString t = token.trimmed();
            if (t.isEmpty()) return;
            queueList->addItem(t);
            queueList->setCurrentRow(queueList->count() - 1);
        };

        auto refreshCandidates = [&]() {
            if (!sourceReadyTried) {
                QString err;
                ensureSourceIndexReady(&err);
                sourceReadyTried = true;
            }
            picks.clear();

            // Local user template directory: write as relative path template/...
            const QDir localTplDir(m_templateDir);
            if (localTplDir.exists()) {
                QDirIterator it(m_templateDir, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    const QString full = it.next();
                    const QString ext = QFileInfo(full).suffix().toLower();
                    if (!isAudioSuffix(ext)) continue;
                    TemplatePick p;
                    const QString rel = localTplDir.relativeFilePath(full).replace('\\', '/');
                    p.valueForJson = QStringLiteral("template/%1").arg(rel);
                    p.display = QFileInfo(rel).completeBaseName();
                    p.localPath = full;
                    picks.push_back(p);
                }
            }

            // Template entries from pak or scanned template sources.
            QSet<QString> seenLower;
            for (const auto &p : std::as_const(picks)) seenLower.insert(p.valueForJson.toLower());
            for (int i = 0; i < m_sources.size(); ++i) {
                const SourceEntry &s = m_sources[i];
                if (!s.isTemplate) continue;
                if (s.baseFileName.isEmpty()) continue;
                const QString val = s.baseFileName;
                if (seenLower.contains(val.toLower())) continue;
                TemplatePick p;
                p.valueForJson = val;
                p.sourceIdx = i;
                p.display = QFileInfo(val).completeBaseName();
                picks.push_back(p);
                seenLower.insert(val.toLower());
            }

            QCollator coll(QLocale(QLocale::Chinese, QLocale::China));
            coll.setCaseSensitivity(Qt::CaseInsensitive);
            coll.setNumericMode(true);
            std::sort(picks.begin(), picks.end(), [&coll](const TemplatePick &a, const TemplatePick &b) {
                const int cmp = coll.compare(a.display, b.display);
                if (cmp != 0) return cmp < 0;
                return coll.compare(a.valueForJson, b.valueForJson) < 0;
            });

            const QString key = searchEdit->text().trimmed().toLower();
            candList->clear();
            for (int i = 0; i < picks.size(); ++i) {
                const TemplatePick &p = picks[i];
                const QString hay = (p.display + QLatin1Char(' ') + p.valueForJson).toLower();
                if (!key.isEmpty() && !hay.contains(key)) continue;
                auto *it = new QListWidgetItem(p.display, candList);
                it->setData(Qt::UserRole, i);
                it->setData(Qt::UserRole + 1, p.valueForJson);
                it->setToolTip(p.valueForJson);
            }
            if (candList->count() > 0) candList->setCurrentRow(0);
        };

        auto loadConfigFile = [&](const QString &cfgPath) {
            currentConfigPath = cfgPath;
            pathLabel->setText(QString::fromUtf8(u8"当前配置：%1").arg(cfgPath));
            autoResources.clear();
            keyByValueLower.clear();
            resourceCounter = 1;
            for (int i = 0; i < seqNames.size(); ++i) {
                if (!seqTable->item(i, 1)) seqTable->setItem(i, 1, new QTableWidgetItem(seqDefaults[i]));
                else seqTable->item(i, 1)->setText(seqDefaults[i]);
            }
            nameEdit->clear();
            engCheck->setChecked(false);

            if (!QFile::exists(cfgPath)) {
                nameEdit->setText(QFileInfo(cfgPath).completeBaseName());
                refreshResourcesTable();
                loadCurrentTypeQueue();
                return;
            }
            QFile f(cfgPath);
            if (!f.open(QIODevice::ReadOnly)) return;
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (!doc.isObject()) return;
            const QJsonObject obj = doc.object();
            nameEdit->setText(obj.value(QStringLiteral("name")).toString(QFileInfo(cfgPath).completeBaseName()));
            engCheck->setChecked(obj.value(QStringLiteral("eng")).toBool(false));

            const QJsonObject resObj = obj.value(QStringLiteral("resources")).toObject();
            QRegularExpression re(QStringLiteral("^res(\\d+)$"));
            for (auto it = resObj.begin(); it != resObj.end(); ++it) {
                if (!it.value().isString()) continue;
                const QString k = it.key().trimmed();
                const QString v = it.value().toString().trimmed();
                if (k.isEmpty() || v.isEmpty()) continue;
                autoResources.insert(k, v);
                keyByValueLower.insert(v.toLower(), k);
                const auto m = re.match(k);
                if (m.hasMatch()) {
                    resourceCounter = qMax(resourceCounter, m.captured(1).toInt() + 1);
                }
            }
            refreshResourcesTable();

            const QJsonObject seqObj = obj.value(QStringLiteral("sequences")).toObject();
            for (int i = 0; i < seqNames.size(); ++i) {
                const QString key = seqNames[i];
                if (!seqObj.contains(key)) continue;
                const QJsonArray arr = seqObj.value(key).toArray();
                QStringList tokens;
                for (const QJsonValue &v : arr) if (v.isString()) tokens << v.toString();
                if (!tokens.isEmpty()) seqTable->item(i, 1)->setText(tokens.join(QStringLiteral(", ")));
            }
            loadCurrentTypeQueue();
        };

        connect(loadBtn, &QPushButton::clicked, &dlg, [&, this]() {
            QString fileName = fileEdit->text().trimmed();
            if (fileName.isEmpty()) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加配置"), QString::fromUtf8(u8"请输入配置文件名"));
                return;
            }
            static const QRegularExpression badRe(QStringLiteral(R"([\\/:*?"<>|])"));
            fileName.replace(badRe, QStringLiteral("_"));
            if (!fileName.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
                fileName += QStringLiteral(".json");
            }
            fileEdit->setText(fileName);
            const QString cfgPath = QDir(m_configDir).absoluteFilePath(fileName);
            loadConfigFile(cfgPath);
            refreshCandidates();
        });

        connect(searchEdit, &QLineEdit::textChanged, &dlg, [refreshCandidates]() { refreshCandidates(); });

        auto importTemplateAudio = [&]() -> bool {
            const QString src = QFileDialog::getOpenFileName(
                &dlg,
                QString::fromUtf8(u8"导入模板音频"),
                m_templateDir,
                QStringLiteral("Audio Files (*.mp3 *.wav *.m4a *.ogg *.flac *.aac *.wma)"));
            if (src.isEmpty()) return false;
            const QString srcClean = QDir::cleanPath(src);
            const QString tplClean = QDir::cleanPath(m_templateDir);
            if (srcClean.toLower().startsWith(tplClean.toLower())) {
                refreshCandidates();
                return true;
            }
            const QString customDir = QDir(m_templateDir).absoluteFilePath(QStringLiteral("custom"));
            QDir().mkpath(customDir);
            QFileInfo fi(src);
            const QString base = fi.completeBaseName();
            const QString ext = fi.suffix().isEmpty() ? QStringLiteral("mp3") : fi.suffix();
            QString target = QDir(customDir).absoluteFilePath(QStringLiteral("%1.%2").arg(base, ext));
            int idx = 1;
            while (QFile::exists(target)) {
                target = QDir(customDir).absoluteFilePath(QStringLiteral("%1_%2.%3").arg(base).arg(idx++).arg(ext));
            }
            if (!QFile::copy(src, target)) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加配置"), QString::fromUtf8(u8"导入模板音频失败"));
                return false;
            }
            refreshCandidates();
            appendLog(QString::fromUtf8(u8"已导入模板音频：%1").arg(QFileInfo(target).fileName()));
            return true;
        };

        connect(importBtn, &QPushButton::clicked, &dlg, [importTemplateAudio]() { importTemplateAudio(); });

        connect(addResBtn, &QPushButton::clicked, &dlg, [&]() {
            const auto *cur = candList->currentItem();
            if (!cur) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加配置"), QString::fromUtf8(u8"请先在左侧选择模板音频"));
                return;
            }
            const int idx = cur->data(Qt::UserRole).toInt();
            if (idx < 0 || idx >= picks.size()) return;
            const QString key = ensureResourceKey(picks[idx].valueForJson);
            for (int row = 0; row < resTable->rowCount(); ++row) {
                const QString k = resTable->item(row, 0) ? resTable->item(row, 0)->text().trimmed() : QString();
                if (k == key) {
                    resTable->setCurrentCell(row, 0);
                    break;
                }
            }
        });

        resTable->onDropValue = [&](const QString &valueText) {
            const QString v = valueText.trimmed();
            if (v.isEmpty()) return;
            int idx = -1;
            for (int i = 0; i < picks.size(); ++i) {
                if (picks[i].valueForJson.compare(v, Qt::CaseInsensitive) == 0 ||
                    picks[i].display.compare(v, Qt::CaseInsensitive) == 0) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) return;
            const QString key = ensureResourceKey(picks[idx].valueForJson);
            for (int row = 0; row < resTable->rowCount(); ++row) {
                const QString k = resTable->item(row, 0) ? resTable->item(row, 0)->text().trimmed() : QString();
                if (k == key) {
                    resTable->setCurrentCell(row, 0);
                    break;
                }
            }
        };

        connect(addResToSeqBtn, &QPushButton::clicked, &dlg, [&]() {
            const int row = resTable->currentRow();
            if (row < 0 || row >= resTable->rowCount()) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加配置"), QString::fromUtf8(u8"请先在资源映射中选中一条"));
                return;
            }
            const QString key = resTable->item(row, 0) ? resTable->item(row, 0)->text().trimmed() : QString();
            appendTokenToQueue(key);
        });

        connect(addConstBtn, &QPushButton::clicked, &dlg, [&]() {
            const auto *cur = constList->currentItem();
            if (!cur) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加配置"), QString::fromUtf8(u8"请先在常量池选择常量"));
                return;
            }
            appendTokenToQueue(cur->text().trimmed());
        });
        connect(constList, &QListWidget::itemDoubleClicked, &dlg, [&](QListWidgetItem *item) {
            if (item) appendTokenToQueue(item->text().trimmed());
        });

        connect(saveCurrentTypeBtn, &QPushButton::clicked, &dlg, [&, this]() {
            saveCurrentTypeToTable();
            appendLog(QString::fromUtf8(u8"已保存当前类型：%1").arg(seqSelect->currentText()));
        });

        connect(seqSelect, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, [&](int) {
            saveCurrentTypeToTable();
            loadCurrentTypeQueue();
        });

        connect(removeTokenBtn, &QPushButton::clicked, &dlg, [&]() {
            const int row = queueList->currentRow();
            if (row < 0 || row >= queueList->count()) return;
            delete queueList->takeItem(row);
            if (row < queueList->count()) queueList->setCurrentRow(row);
            else if (queueList->count() > 0) queueList->setCurrentRow(queueList->count() - 1);
        });
        connect(clearQueueBtn, &QPushButton::clicked, &dlg, [&]() {
            queueList->clear();
        });
        connect(moveUpBtn, &QPushButton::clicked, &dlg, [&]() {
            const int row = queueList->currentRow();
            if (row <= 0) return;
            auto *item = queueList->takeItem(row);
            queueList->insertItem(row - 1, item);
            queueList->setCurrentRow(row - 1);
        });
        connect(moveDownBtn, &QPushButton::clicked, &dlg, [&]() {
            const int row = queueList->currentRow();
            if (row < 0 || row >= queueList->count() - 1) return;
            auto *item = queueList->takeItem(row);
            queueList->insertItem(row + 1, item);
            queueList->setCurrentRow(row + 1);
        });

        connect(removeResBtn, &QPushButton::clicked, &dlg, [&, this]() {
            const int row = resTable->currentRow();
            if (row < 0 || row >= resTable->rowCount()) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加配置"), QString::fromUtf8(u8"请先选中要删除的资源"));
                return;
            }
            const QString key = resTable->item(row, 0) ? resTable->item(row, 0)->text().trimmed() : QString();
            if (key.isEmpty()) return;
            QString removedValue = autoResources.value(key);
            autoResources.remove(key);
            keyByValueLower.remove(removedValue.toLower());

            for (int i = 0; i < seqTable->rowCount(); ++i) {
                const QString raw = seqTable->item(i, 1) ? seqTable->item(i, 1)->text() : QString();
                QStringList tokens = raw.split(splitRe, Qt::SkipEmptyParts);
                tokens.removeAll(key);
                if (seqTable->item(i, 1)) seqTable->item(i, 1)->setText(tokens.join(QStringLiteral(", ")));
            }
            refreshResourcesTable();
            loadCurrentTypeQueue();
        });

        connect(previewBtn, &QPushButton::clicked, &dlg, [&, this]() {
            const auto *cur = candList->currentItem();
            if (!cur) return;
            const int idx = cur->data(Qt::UserRole).toInt();
            if (idx < 0 || idx >= picks.size()) return;
            const TemplatePick &p = picks[idx];
            QString err;
            QByteArray bytes;
            QString hint = QFileInfo(p.valueForJson).fileName();
            if (p.sourceIdx >= 0 && p.sourceIdx < m_sources.size()) {
                bytes = sourceBytes(m_sources[p.sourceIdx], &err);
                hint = m_sources[p.sourceIdx].baseFileName;
            } else if (!p.localPath.isEmpty()) {
                QFile f(p.localPath);
                if (f.open(QIODevice::ReadOnly)) bytes = f.readAll();
            } else {
                QString rel = p.valueForJson;
                if (rel.startsWith(QStringLiteral("template/"))) rel = rel.mid(QStringLiteral("template/").size());
                const QString localPath = QDir(m_templateDir).absoluteFilePath(rel);
                QFile f(localPath);
                if (f.open(QIODevice::ReadOnly)) bytes = f.readAll();
            }
            if (bytes.isEmpty()) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加配置"),
                                     err.isEmpty() ? QString::fromUtf8(u8"模板音频读取失败") : err);
                return;
            }
            if (!playSourceBytes(bytes, QString::fromUtf8(u8"模板试听: %1").arg(p.valueForJson), hint, &err, false)) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加配置"),
                                     err.isEmpty() ? QString::fromUtf8(u8"试听失败") : err);
                return;
            }
        });

        connect(saveBtn, &QPushButton::clicked, &dlg, [&, this]() {
            if (currentConfigPath.isEmpty()) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加配置"), QString::fromUtf8(u8"请先创建或读取配置文件"));
                return;
            }
            saveCurrentTypeToTable();
            QJsonObject rootObj;
            const QString displayName = nameEdit->text().trimmed().isEmpty()
                                            ? QFileInfo(currentConfigPath).completeBaseName()
                                            : nameEdit->text().trimmed();
            rootObj.insert(QStringLiteral("name"), displayName);
            rootObj.insert(QStringLiteral("eng"), engCheck->isChecked());

            QSet<QString> usedResourceKeys;
            for (int i = 0; i < seqTable->rowCount(); ++i) {
                const QString raw = (seqTable->item(i, 1) ? seqTable->item(i, 1)->text() : QString()).trimmed();
                const QStringList tokens = raw.split(splitRe, Qt::SkipEmptyParts);
                for (const QString &t : tokens) {
                    if (!t.startsWith(QLatin1Char('$'))) usedResourceKeys.insert(t);
                }
            }

            QJsonObject resObj;
            for (auto it = autoResources.begin(); it != autoResources.end(); ++it) {
                if (usedResourceKeys.contains(it.key()) &&
                    !it.key().trimmed().isEmpty() &&
                    !it.value().trimmed().isEmpty()) {
                    resObj.insert(it.key(), it.value());
                }
            }
            rootObj.insert(QStringLiteral("resources"), resObj);

            QJsonObject seqObj;
            for (int i = 0; i < seqTable->rowCount(); ++i) {
                const QString seqName = (seqTable->item(i, 0) ? seqTable->item(i, 0)->text() : QString()).trimmed();
                const QString raw = (seqTable->item(i, 1) ? seqTable->item(i, 1)->text() : QString()).trimmed();
                QStringList tokens = raw.split(splitRe, Qt::SkipEmptyParts);
                QJsonArray arr;
                for (const QString &t : tokens) arr.push_back(t);
                if (!seqName.isEmpty()) seqObj.insert(seqName, arr);
            }
            rootObj.insert(QStringLiteral("sequences"), seqObj);

            QDir().mkpath(m_configDir);
            QFile out(currentConfigPath);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QMessageBox::warning(&dlg, QString::fromUtf8(u8"添加配置"), QString::fromUtf8(u8"写入配置失败"));
                return;
            }
            out.write(QJsonDocument(rootObj).toJson(QJsonDocument::Indented));
            out.close();

            m_savedCfgName = displayName;
            saveUserPreferences();
            reloadInputLists();
            appendLog(QString::fromUtf8(u8"配置保存完成：%1").arg(QFileInfo(currentConfigPath).fileName()));
            QMessageBox::information(&dlg, QString::fromUtf8(u8"添加配置"), QString::fromUtf8(u8"配置保存成功"));
        });

        connect(helpBtn, &QPushButton::clicked, &dlg, [&]() {
            auto *help = new QDialog(&dlg);
            help->setAttribute(Qt::WA_DeleteOnClose);
            help->setWindowTitle(QString::fromUtf8(u8"添加配置 - 使用说明"));
            help->resize(620, 420);
            auto *layout = new QVBoxLayout(help);
            auto *text = new QPlainTextEdit(help);
            text->setReadOnly(true);
            text->setPlainText(QString::fromUtf8(
                u8"1. 输入配置文件名并点击“创建/读取”。\n"
                u8"2. 左侧模板可点“加入资源”，或直接拖拽到资源映射表。\n"
                u8"3. 资源映射里选中某条后，点“加入资源到队列”放入当前类别。\n"
                u8"4. 右侧常量池选中后点“加入常量”，追加到当前队列。\n"
                u8"5. 中间队列可拖拽排序，也可上移/下移；点“保存当前类型”写入该类别。\n"
                u8"6. 四类默认空序列，编辑完成后点“保存配置”写入 json。"));
            auto *close = new QPushButton(QString::fromUtf8(u8"关闭"), help);
            layout->addWidget(text, 1);
            layout->addWidget(close, 0, Qt::AlignRight);
            connect(close, &QPushButton::clicked, help, &QDialog::close);
            help->setModal(false);
            help->show();
            help->raise();
            help->activateWindow();
        });

        connect(sampleBtn, &QPushButton::clicked, &dlg, [&, this]() {
            if (fileEdit->text().trimmed().isEmpty()) {
                fileEdit->setText(QString::fromUtf8(u8"示例配置.json"));
            }
            QString cfgName = fileEdit->text().trimmed();
            if (!cfgName.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
                cfgName += QStringLiteral(".json");
                fileEdit->setText(cfgName);
            }
            currentConfigPath = QDir(m_configDir).absoluteFilePath(cfgName);
            pathLabel->setText(QString::fromUtf8(u8"当前配置：%1（示例，未保存）").arg(currentConfigPath));
            nameEdit->setText(QString::fromUtf8(u8"示例模板"));
            engCheck->setChecked(false);

            autoResources.clear();
            keyByValueLower.clear();
            resourceCounter = 1;
            const QString k1 = ensureResourceKey(QString::fromUtf8(u8"开往.mp3"));
            const QString k2 = ensureResourceKey(QString::fromUtf8(u8"起点站.mp3"));
            const QString k3 = ensureResourceKey(QString::fromUtf8(u8"终点站.mp3"));

            if (seqTable->rowCount() >= 4) {
                if (seqTable->item(0, 1)) seqTable->item(0, 1)->setText(QStringLiteral("%1, $CURRENT_STATION").arg(k2));
                if (seqTable->item(1, 1)) seqTable->item(1, 1)->setText(QStringLiteral("$CURRENT_STATION, %1, $TERMINAL").arg(k1));
                if (seqTable->item(2, 1)) seqTable->item(2, 1)->setText(QStringLiteral("$NEXT_STATION"));
                if (seqTable->item(3, 1)) seqTable->item(3, 1)->setText(QStringLiteral("%1, $TERMINAL").arg(k3));
            }
            refreshResourcesTable();
            loadCurrentTypeQueue();
            appendLog(QString::fromUtf8(u8"配置编辑器已填充示例数据（未保存）"));
        });

        connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        refreshCandidates();
        loadCurrentTypeQueue();
        dlg.exec();
    }

    void buildPromptButtons(QVBoxLayout *panelLayout, QWidget *parentWidget) {
        auto *promptTitle = new QLabel(QString::fromUtf8(u8"提示音快捷播放"), parentWidget);
        promptTitle->setStyleSheet(QStringLiteral("font-size:15px; color:#b8c3d3;"));
        panelLayout->addWidget(promptTitle);

        auto *promptRow = new QHBoxLayout();
        const QVector<QPair<QString, int>> promptDefs = {
            {QString::fromUtf8(u8"提示1"), 1},
            {QString::fromUtf8(u8"提示2"), 2},
            {QString::fromUtf8(u8"转弯"), 3},
            {QString::fromUtf8(u8"让座"), 4},
            {QString::fromUtf8(u8"提示3"), 5},
            {QString::fromUtf8(u8"提示4"), 6},
            {QString::fromUtf8(u8"进选线"), -1},
            {QString::fromUtf8(u8"切线路"), -2},
        };
        for (const auto &def : promptDefs) {
            auto *btn = new QPushButton(def.first, parentWidget);
            btn->setMinimumHeight(34);
            promptRow->addWidget(btn);
            m_promptButtons.push_back(btn);
            connect(btn, &QPushButton::clicked, this, [this, code = def.second]() {
                if (code > 0) {
                    playPromptByIndex(code);
                } else if (code == -1) {
                    playNamedPrompt(QString::fromUtf8(u8"进入公交线路选择模式"));
                } else {
                    playNamedPrompt(QString::fromUtf8(u8"切换线路为"));
                }
            });
        }
        panelLayout->addLayout(promptRow);
    }

    void buildUi() {
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(10);

        m_topMenuBar = new QMenuBar(this);
        auto *previewMenu = m_topMenuBar->addMenu(QString::fromUtf8(u8"浏览"));
        m_previewAction = previewMenu->addAction(QString::fromUtf8(u8"浏览全部..."));
        auto *addMenu = m_topMenuBar->addMenu(QString::fromUtf8(u8"添加"));
        m_addLineAction = addMenu->addAction(QString::fromUtf8(u8"添加线路"));
        m_addConfigAction = addMenu->addAction(QString::fromUtf8(u8"添加配置"));
        auto *helpMenu = m_topMenuBar->addMenu(QString::fromUtf8(u8"帮助"));
        m_helpAction = helpMenu->addAction(QString::fromUtf8(u8"使用帮助"));
        m_aboutAction = helpMenu->addAction(QString::fromUtf8(u8"关于"));
        root->setMenuBar(m_topMenuBar);

        auto *top = new QHBoxLayout();
        m_cfgCombo = new QComboBox(this);
        m_lineCombo = new QComboBox(this);
        m_reloadBtn = new QPushButton(QString::fromUtf8(u8"刷新"), this);
        m_loadBtn = new QPushButton(QString::fromUtf8(u8"加载配置与线路"), this);
        m_clearCacheBtn = new QPushButton(QString::fromUtf8(u8"删除缓存"), this);
        top->addWidget(new QLabel(QString::fromUtf8(u8"配置:"), this));
        top->addWidget(m_cfgCombo, 2);
        top->addWidget(new QLabel(QString::fromUtf8(u8"线路:"), this));
        top->addWidget(m_lineCombo, 2);
        top->addWidget(m_reloadBtn);
        top->addWidget(m_loadBtn);
        top->addWidget(m_clearCacheBtn);
        root->addLayout(top);

        m_statusLabel = new QLabel(QString::fromUtf8(u8"就绪"), this);
        m_statusLabel->setStyleSheet(QStringLiteral("color:#9eb5d4;"));
        root->addWidget(m_statusLabel);

        m_loadProgress = new QProgressBar(this);
        m_loadProgress->setRange(0, 100);
        m_loadProgress->setValue(0);
        root->addWidget(m_loadProgress);

        m_logView = new QPlainTextEdit(this);
        m_logView->setReadOnly(true);
        m_logView->setMaximumBlockCount(600);
        m_logView->setMinimumHeight(120);
        m_logView->setPlaceholderText(QString::fromUtf8(u8"加载与合成日志会显示在这里"));
        root->addWidget(m_logView);
        m_clearLogBtn = new QToolButton(m_logView);
        m_clearLogBtn->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
        m_clearLogBtn->setToolTip(QString::fromUtf8(u8"清除日志"));
        m_clearLogBtn->setAutoRaise(true);
        m_clearLogBtn->setCursor(Qt::PointingHandCursor);
        m_clearLogBtn->setStyleSheet(QStringLiteral(
            "QToolButton { background: rgba(43,49,59,190); border: 1px solid #4b5566; border-radius: 10px; padding: 2px; }"
            "QToolButton:hover { background: rgba(70,78,92,210); }"));
        placeLogClearButton();

        auto *content = new QHBoxLayout();
        m_playlist = new QListWidget(this);
        m_playlist->setMinimumWidth(430);
        content->addWidget(m_playlist, 3);

        auto *panel = new QWidget(this);
        auto *panelLayout = new QVBoxLayout(panel);
        auto *title = new QLabel(QString::fromUtf8(u8"播放器"), panel);
        title->setStyleSheet(QStringLiteral("font-size:22px; font-weight:700;"));
        panelLayout->addWidget(title);
        buildPromptButtons(panelLayout, panel);
        m_nowPlayingLabel = new QLabel(QString::fromUtf8(u8"当前: （无）"), panel);
        panelLayout->addWidget(m_nowPlayingLabel);
        m_progress = new QSlider(Qt::Horizontal, panel);
        m_progress->setRange(0, 0);
        panelLayout->addWidget(m_progress);
        m_timeLabel = new QLabel(QStringLiteral("00:00 / 00:00"), panel);
        panelLayout->addWidget(m_timeLabel);

        auto *controlsRow = new QHBoxLayout();
        auto *btnRow = new QHBoxLayout();
        m_prevBtn = new QToolButton(panel);
        m_playPauseBtn = new QToolButton(panel);
        m_nextBtn = new QToolButton(panel);
        m_modeBtn = new QToolButton(panel);
        m_volumeBtn = new QToolButton(panel);
        styleControlButton(m_prevBtn);
        styleControlButton(m_playPauseBtn);
        styleControlButton(m_nextBtn);
        styleControlButton(m_modeBtn);
        styleControlButton(m_volumeBtn);
        m_prevBtn->setIcon(monoStyleIcon(QStyle::SP_MediaSkipBackward));
        m_prevBtn->setToolTip(QString::fromUtf8(u8"上一条"));
        m_nextBtn->setIcon(monoStyleIcon(QStyle::SP_MediaSkipForward));
        m_nextBtn->setToolTip(QString::fromUtf8(u8"下一条"));
        updatePlayPauseButton(false);
        updatePlaybackModeButton();
        m_volumeBtn->setToolTip(QString::fromUtf8(u8"音量"));
        btnRow->addWidget(m_prevBtn);
        btnRow->addWidget(m_playPauseBtn);
        btnRow->addWidget(m_nextBtn);
        btnRow->addWidget(m_modeBtn);
        btnRow->addWidget(m_volumeBtn);
        controlsRow->addLayout(btnRow, 1);

        m_volumePopup = new QWidget(this, Qt::Popup | Qt::FramelessWindowHint);
        m_volumePopup->setAttribute(Qt::WA_ShowWithoutActivating);
        m_volumePopup->setAttribute(Qt::WA_TranslucentBackground);
        m_volumePopup->setObjectName(QStringLiteral("volumePopupRoot"));
        auto *popupRootLayout = new QVBoxLayout(m_volumePopup);
        popupRootLayout->setContentsMargins(0, 0, 0, 0);
        popupRootLayout->setSpacing(0);
        auto *popupCard = new QWidget(m_volumePopup);
        popupCard->setObjectName(QStringLiteral("volumePopupCard"));
        popupRootLayout->addWidget(popupCard);
        popupCard->setStyleSheet(QStringLiteral(
            "QWidget#volumePopupCard { background: #252b34; border: 1px solid #3a4452; border-radius: 8px; }"
            "QLabel { color: #d9e3f2; font-weight: 600; }"
            "QSlider::groove:vertical { background: #1f2530; width: 6px; border-radius: 3px; }"
            "QSlider::sub-page:vertical { background: #5f87c4; border-radius: 3px; }"
            "QSlider::add-page:vertical { background: #2e3642; border-radius: 3px; }"
            "QSlider::handle:vertical { background: #d7e4ff; height: 14px; margin: -4px -6px; border-radius: 7px; border: 1px solid #8ba8d6; }"));
        auto *volumePopupLayout = new QVBoxLayout(popupCard);
        volumePopupLayout->setContentsMargins(8, 8, 8, 8);
        volumePopupLayout->setSpacing(6);
        m_volumeValueLabel = new QLabel(QStringLiteral("100%"), popupCard);
        m_volumeValueLabel->setAlignment(Qt::AlignHCenter);
        m_volumeSlider = new QSlider(Qt::Vertical, popupCard);
        m_volumeSlider->setRange(0, 100);
        m_volumeSlider->setValue(100);
        m_volumeSlider->setSingleStep(1);
        m_volumeSlider->setPageStep(5);
        m_volumeSlider->setFixedHeight(120);
        volumePopupLayout->addWidget(m_volumeValueLabel);
        volumePopupLayout->addWidget(m_volumeSlider, 0, Qt::AlignHCenter);
        updateVolumeValueUi(100, false);

        panelLayout->addLayout(controlsRow);
        panelLayout->addStretch();
        content->addWidget(panel, 5);
        root->addLayout(content);
    }

    void connectSignals() {
        if (m_previewAction) {
            connect(m_previewAction, &QAction::triggered, this, [this]() { openPreviewDialog(); });
        }
        if (m_addLineAction) {
            connect(m_addLineAction, &QAction::triggered, this, [this]() { openAddLineDialog(); });
        }
        if (m_addConfigAction) {
            connect(m_addConfigAction, &QAction::triggered, this, [this]() { openAddConfigDialog(); });
        }
        if (m_helpAction) {
            connect(m_helpAction, &QAction::triggered, this, [this]() { openUsageHelpDialog(); });
        }
        if (m_aboutAction) {
            connect(m_aboutAction, &QAction::triggered, this, [this]() { openAboutDialog(); });
        }
        connect(m_reloadBtn, &QPushButton::clicked, this, [this]() { reloadInputLists(); });
        connect(m_loadBtn, &QPushButton::clicked, this, [this]() { loadSelectedConfigAndLine(); });
        connect(m_clearCacheBtn, &QPushButton::clicked, this, [this]() { clearGeneratedCache(); });
        if (m_clearLogBtn) {
            connect(m_clearLogBtn, &QToolButton::clicked, this, [this]() {
                if (m_logView) m_logView->clear();
            });
        }
        connect(m_playlist, &QListWidget::currentRowChanged, this, [this](int row) { if (row >= 0) playTrack(row); });
        connect(m_prevBtn, &QToolButton::clicked, this, [this]() { playPrev(); });
        connect(m_nextBtn, &QToolButton::clicked, this, [this]() { playNext(); });
        connect(m_playPauseBtn, &QToolButton::clicked, this, [this]() { togglePlayPause(); });
        connect(m_modeBtn, &QToolButton::clicked, this, [this]() { cyclePlaybackMode(); });
        if (m_volumeBtn) {
            connect(m_volumeBtn, &QToolButton::clicked, this, [this]() { toggleVolumePopup(); });
        }
        if (m_volumeSlider) {
            connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
                const qreal appVolume = qBound(0.0, static_cast<qreal>(value) / 100.0, 1.0);
                m_audio->setVolume(appVolume);
                if (!m_syncingSystemVolume) writeSystemVolumePercent(value);
                updateVolumeValueUi(value, m_volumeSlider->isSliderDown());
            });
            connect(m_volumeSlider, &QSlider::sliderMoved, this, [this](int value) {
                updateVolumeValueUi(value, true);
            });
        }

        connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
            m_progress->setRange(0, static_cast<int>(duration));
            m_timeLabel->setText(formatMs(m_player->position()) + QStringLiteral(" / ") + formatMs(duration));
        });
        connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
            if (!m_userSeeking) m_progress->setValue(static_cast<int>(position));
            m_timeLabel->setText(formatMs(position) + QStringLiteral(" / ") + formatMs(m_player->duration()));
        });
        connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState st) {
            updatePlayPauseButton(st == QMediaPlayer::PlayingState);
        });
        connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus st) {
            if (st == QMediaPlayer::EndOfMedia) handleEndOfMedia();
        });
        connect(m_progress, &QSlider::sliderPressed, this, [this]() { m_userSeeking = true; });
        connect(m_progress, &QSlider::sliderReleased, this, [this]() {
            m_userSeeking = false;
            m_player->setPosition(m_progress->value());
        });
    }

    QString detectRuntimeRoot() const {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString leaf = QFileInfo(appDir).fileName().toLower();
        if (leaf == QStringLiteral("release") || leaf == QStringLiteral("debug")) {
            return QDir(appDir).absoluteFilePath(QStringLiteral(".."));
        }
        return appDir;
    }

    void initRuntimeDirs() {
        m_runtimeRoot = detectRuntimeRoot();
        m_linesDir = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("lines"));
        m_configDir = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("config"));
        m_packsDir = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("packs"));
        m_concatDir = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("../00concat"));
        m_concatEngDir = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("../00concatEng"));
        m_templateDir = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("template"));
        m_templateLegacyDir = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("../template"));
        m_promptDir = QDir(m_runtimeRoot).absoluteFilePath(QString::fromUtf8(u8"../提示"));

        auto dirHasTemplateJson = [](const QString &dirPath) -> bool {
            const QDir d(dirPath);
            if (!d.exists()) return false;
            const QStringList jsonFiles = d.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files, QDir::Name);
            for (const QString &name : jsonFiles) {
                if (name.compare(QStringLiteral("pack_manifest.json"), Qt::CaseInsensitive) == 0) continue;
                QFile file(d.absoluteFilePath(name));
                if (!file.open(QIODevice::ReadOnly)) continue;
                const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
                if (!doc.isObject()) continue;
                const QJsonObject seq = doc.object().value(QStringLiteral("sequences")).toObject();
                if (!seq.isEmpty()) return true;
            }
            return false;
        };
        auto dirHasLines = [](const QString &dirPath) -> bool {
            const QDir d(dirPath);
            return d.exists() &&
                   !d.entryList(QStringList() << QStringLiteral("*.xlsx") << QStringLiteral("*.txt"), QDir::Files).isEmpty();
        };
        auto writeFallbackConfigIfMissing = [&](const QString &configDirPath) {
            if (dirHasTemplateJson(configDirPath)) return;
            const QString fallbackPath = QDir(configDirPath).absoluteFilePath(QStringLiteral("fallback_template.json"));
            if (QFile::exists(fallbackPath)) return;
            QJsonObject root;
            root.insert(QStringLiteral("name"), QString::fromUtf8(u8"默认模板"));
            root.insert(QStringLiteral("eng"), false);
            root.insert(QStringLiteral("resources"), QJsonObject());
            QJsonObject seq;
            seq.insert(QStringLiteral("start_station"), QJsonArray{QStringLiteral("$CURRENT_STATION")});
            seq.insert(QStringLiteral("enter_station"), QJsonArray{QStringLiteral("$CURRENT_STATION")});
            seq.insert(QStringLiteral("next_station"), QJsonArray{QStringLiteral("$NEXT_STATION")});
            seq.insert(QStringLiteral("terminal_station"), QJsonArray{QStringLiteral("$TERMINAL")});
            root.insert(QStringLiteral("sequences"), seq);
            QDir().mkpath(configDirPath);
            QFile out(fallbackPath);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            }
        };
        auto extractZipToDir = [&](const QString &zipPath, const QString &destDir, QString *err) -> bool {
            QFile file(zipPath);
            if (!file.open(QIODevice::ReadOnly)) {
                if (err) *err = QStringLiteral("Cannot open seed zip: %1").arg(zipPath);
                return false;
            }
            QZipReader zip(&file);
            if (zip.status() != QZipReader::NoError) {
                if (err) *err = QStringLiteral("Invalid seed zip: %1").arg(zipPath);
                return false;
            }
            QDir().mkpath(destDir);
            for (const QZipReader::FileInfo &fi : zip.fileInfoList()) {
                const QString targetPath = QDir(destDir).absoluteFilePath(fi.filePath);
                if (fi.isDir) {
                    QDir().mkpath(targetPath);
                    continue;
                }
                const QByteArray bytes = zip.fileData(fi.filePath);
                if (bytes.isEmpty()) continue;
                QDir().mkpath(QFileInfo(targetPath).path());
                QFile out(targetPath);
                if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(bytes) != bytes.size()) {
                    if (err) *err = QStringLiteral("Cannot extract seed file: %1").arg(targetPath);
                    return false;
                }
            }
            return true;
        };

        auto copyResourceToFileIfMissing = [](const QString &resourcePath, const QString &destPath, QString *err) -> bool {
            if (QFile::exists(destPath)) return true;
            QFile in(resourcePath);
            if (!in.exists()) return false;
            if (!in.open(QIODevice::ReadOnly)) {
                if (err) *err = QStringLiteral("Cannot open embedded resource: %1").arg(resourcePath);
                return false;
            }
            const QByteArray bytes = in.readAll();
            if (bytes.isEmpty()) return false;
            QDir().mkpath(QFileInfo(destPath).path());
            QFile out(destPath);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(bytes) != bytes.size()) {
                if (err) *err = QStringLiteral("Cannot write extracted resource: %1").arg(destPath);
                return false;
            }
            return true;
        };

        auto seedFromEmbeddedPayload = [&](QString *err) {
            QDir().mkpath(m_configDir);
            QDir().mkpath(m_linesDir);
            QDir().mkpath(m_packsDir);

            copyResourceToFileIfMissing(QStringLiteral(":/payload/seed_config.zip"),
                                        QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("seed_config.zip")),
                                        err);
            copyResourceToFileIfMissing(QStringLiteral(":/payload/seed_lines.zip"),
                                        QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("seed_lines.zip")),
                                        err);

            const QDir resPackDir(QStringLiteral(":/payload/packs"));
            if (resPackDir.exists()) {
                const QStringList paks = resPackDir.entryList(QStringList() << QStringLiteral("*.pak"), QDir::Files, QDir::Name);
                for (const QString &pakName : paks) {
                    const QString resPath = QStringLiteral(":/payload/packs/%1").arg(pakName);
                    const QString outPath = QDir(m_packsDir).absoluteFilePath(pakName);
                    copyResourceToFileIfMissing(resPath, outPath, err);
                }
            }
        };

        // On packaged builds, bundled payload/seed archives bootstrap editable config/lines.
        const QString seedConfigZip = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("seed_config.zip"));
        const QString seedLinesZip = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("seed_lines.zip"));
        QString seedErr;
        seedFromEmbeddedPayload(&seedErr);
        if (QFile::exists(seedConfigZip) && !dirHasTemplateJson(m_configDir)) {
            extractZipToDir(seedConfigZip, m_configDir, &seedErr);
        }
        if (QFile::exists(seedLinesZip) && !dirHasLines(m_linesDir)) {
            extractZipToDir(seedLinesZip, m_linesDir, &seedErr);
        }
        QDir().mkpath(m_configDir);
        QDir().mkpath(m_linesDir);
        QDir().mkpath(m_packsDir);
        QDir().mkpath(m_templateDir);
        writeFallbackConfigIfMissing(m_configDir);
    }

    bool parseTemplateConfig(const QString &path, TemplateConfig *cfg, QString *error) const {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Cannot read template: %1").arg(path);
            return false;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) return false;
        const QJsonObject obj = doc.object();
        const QJsonObject seq = obj.value(QStringLiteral("sequences")).toObject();
        if (seq.isEmpty()) return false;

        cfg->filePath = path;
        cfg->name = obj.value(QStringLiteral("name")).toString(QFileInfo(path).completeBaseName());
        cfg->hasEnglish = obj.value(QStringLiteral("eng")).toBool(false);
        cfg->resources.clear();
        cfg->sequences.clear();

        const QJsonObject res = obj.value(QStringLiteral("resources")).toObject();
        for (auto it = res.begin(); it != res.end(); ++it) {
            if (it.value().isString()) cfg->resources.insert(it.key(), it.value().toString());
        }
        for (auto it = seq.begin(); it != seq.end(); ++it) {
            QStringList list;
            const QJsonArray arr = it.value().toArray();
            for (const QJsonValue &v : arr) if (v.isString()) list << v.toString();
            cfg->sequences.insert(it.key(), list);
        }
        return true;
    }

    PackManifest loadPackManifest() const {
        PackManifest m;
        const QString path = QDir(m_configDir).absoluteFilePath(QStringLiteral("pack_manifest.json"));
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            if (doc.isObject()) {
                const QJsonObject obj = doc.object();
                const QString key = obj.value(QStringLiteral("key")).toString();
                if (!key.isEmpty()) m.key = key;
                for (const QJsonValue &v : obj.value(QStringLiteral("packages")).toArray()) {
                    if (v.isString()) m.packages << v.toString();
                }
                const QJsonObject aliasRoot = obj.value(QStringLiteral("aliases")).toObject();
                for (auto pkgIt = aliasRoot.begin(); pkgIt != aliasRoot.end(); ++pkgIt) {
                    const QJsonObject mapObj = pkgIt.value().toObject();
                    QHash<QString, QString> map;
                    for (auto it = mapObj.begin(); it != mapObj.end(); ++it) {
                        if (it.value().isString()) {
                            map.insert(it.key().toLower(), it.value().toString());
                        }
                    }
                    if (!map.isEmpty()) {
                        m.aliasesByPackageLower.insert(pkgIt.key().toLower(), map);
                    }
                }
                const QJsonObject kindsRoot = obj.value(QStringLiteral("kinds")).toObject();
                for (auto it = kindsRoot.begin(); it != kindsRoot.end(); ++it) {
                    if (it.value().isString()) {
                        m.kindsByPackageLower.insert(it.key().toLower(), it.value().toString().toLower());
                    }
                }
            }
        }
        if (m.packages.isEmpty()) {
            const QDir packs(m_packsDir);
            for (const QString &n : packs.entryList(QStringList() << QStringLiteral("*.pak"), QDir::Files, QDir::Name)) {
                m.packages << n;
            }
        }
        return m;
    }

    void reloadInputLists() {
        if (m_cfgCombo && m_cfgCombo->currentIndex() >= 0) {
            m_savedCfgName = m_cfgCombo->currentText();
        }
        if (m_lineCombo && m_lineCombo->currentIndex() >= 0) {
            m_savedLineName = m_lineCombo->currentText();
            m_savedLinePath = m_lineCombo->currentData().toString();
        }
        stopAndDetachPlayer();
        m_tracks.clear();
        m_playlist->clear();
        m_templates.clear();
        m_cfgCombo->clear();
        m_lineCombo->clear();
        if (m_loadProgress) m_loadProgress->setValue(0);

        QSet<QString> visited;
        QStringList cfgDirs;
        cfgDirs << m_configDir;

        for (const QString &cfgDirPath : cfgDirs) {
            const QDir cfgDir(cfgDirPath);
            for (const QString &name : cfgDir.entryList(QStringList() << QStringLiteral("*.json"), QDir::Files, QDir::Name)) {
                if (name.compare(QStringLiteral("pack_manifest.json"), Qt::CaseInsensitive) == 0) continue;
                const QString full = cfgDir.absoluteFilePath(name);
                const QString norm = QDir::cleanPath(full).toLower();
                if (visited.contains(norm)) continue;
                visited.insert(norm);
                TemplateConfig cfg;
                QString err;
                if (parseTemplateConfig(full, &cfg, &err)) {
                    m_templates.push_back(cfg);
                    m_cfgCombo->addItem(cfg.name);
                }
            }
        }

        struct LineItem {
            QString displayName;
            QString absolutePath;
            bool hasNumber = false;
            qint64 absNumber = std::numeric_limits<qint64>::max();
            qint64 rawNumber = 0;
        };
        const QDir lineDir(m_linesDir);
        QVector<LineItem> lineItems;
        for (const QString &n : lineDir.entryList(QStringList() << QStringLiteral("*.xlsx") << QStringLiteral("*.txt"),
                                                   QDir::Files, QDir::Name)) {
            LineItem item;
            item.displayName = QFileInfo(n).completeBaseName();
            item.absolutePath = lineDir.absoluteFilePath(n);
            qint64 number = 0;
            if (parseFirstInteger(item.displayName, &number)) {
                item.hasNumber = true;
                item.rawNumber = number;
                item.absNumber = qAbs(number);
            }
            lineItems.push_back(item);
        }
        QCollator collator;
        collator.setNumericMode(true);
        std::sort(lineItems.begin(), lineItems.end(), [&collator](const LineItem &a, const LineItem &b) {
            if (a.hasNumber != b.hasNumber) return a.hasNumber;
            if (a.hasNumber && b.hasNumber) {
                if (a.absNumber != b.absNumber) return a.absNumber < b.absNumber;
                if (a.rawNumber != b.rawNumber) return a.rawNumber < b.rawNumber;
            }
            return collator.compare(a.displayName, b.displayName) < 0;
        });
        for (const LineItem &item : std::as_const(lineItems)) {
            m_lineCombo->addItem(item.displayName, item.absolutePath);
        }
        applySavedSelections();

        if (m_templates.isEmpty()) m_statusLabel->setText(QString::fromUtf8(u8"未在 ./config 找到模板"));
        else if (m_lineCombo->count() == 0) m_statusLabel->setText(QString::fromUtf8(u8"未在 ./lines 找到线路文件"));
        else m_statusLabel->setText(QString::fromUtf8(u8"就绪"));
        appendLog(QString::fromUtf8(u8"配置与线路列表已刷新"));
    }

    bool createSessionDir() {
        cleanupSession();
        const QString tempRoot = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("_runtime_tmp/SecureAudioPlayer"));
        if (!QDir().mkpath(tempRoot)) return false;
        const QString unique = QStringLiteral("%1_%2")
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QDateTime::currentMSecsSinceEpoch());
        m_sessionDir = QDir(tempRoot).absoluteFilePath(unique);
        if (!QDir().mkpath(m_sessionDir)) return false;

        QByteArray nonce(32, Qt::Uninitialized);
        for (int i = 0; i < nonce.size(); ++i) nonce[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
        const QByteArray seed = nonce + m_packKey.toUtf8() +
                                QByteArray::number(QCoreApplication::applicationPid()) +
                                QByteArray::number(QDateTime::currentMSecsSinceEpoch());
        m_sessionBlobKey = QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex());
        return true;
    }

    bool ensureSessionDirReady() {
        if (!m_sessionDir.isEmpty()) {
            if (m_sessionBlobKey.isEmpty()) {
                QByteArray seed = m_packKey.toUtf8() +
                                  QByteArray::number(QCoreApplication::applicationPid()) +
                                  QByteArray::number(QDateTime::currentMSecsSinceEpoch());
                m_sessionBlobKey = QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex());
            }
            return true;
        }
        const QString tempRoot = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("_runtime_tmp/SecureAudioPlayer"));
        if (!QDir().mkpath(tempRoot)) return false;
        const QString unique = QStringLiteral("%1_%2")
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QDateTime::currentMSecsSinceEpoch());
        m_sessionDir = QDir(tempRoot).absoluteFilePath(unique);
        if (!QDir().mkpath(m_sessionDir)) return false;

        QByteArray nonce(32, Qt::Uninitialized);
        for (int i = 0; i < nonce.size(); ++i) nonce[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
        const QByteArray seed = nonce + m_packKey.toUtf8() +
                                QByteArray::number(QCoreApplication::applicationPid()) +
                                QByteArray::number(QDateTime::currentMSecsSinceEpoch());
        m_sessionBlobKey = QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex());
        return true;
    }

    QString sourceKey(const SourceEntry &s) const {
        if (!s.encryptedBlobPath.isEmpty()) {
            return QStringLiteral("enc|%1").arg(QDir::cleanPath(s.encryptedBlobPath).toLower());
        }
        if (!s.diskPath.isEmpty()) {
            return QStringLiteral("disk|%1").arg(QDir::cleanPath(s.diskPath).toLower());
        }
        return QStringLiteral("%1|%2").arg(s.packageIndex).arg(s.zipEntryPath);
    }

    bool loadPackagesForSession(const PackManifest &manifest, QString *error) {
        m_packages.clear();
        m_sources.clear();
        m_indicesByLowerFileName.clear();
        m_indicesByLowerStem.clear();
        m_bestResourceByLowerPath.clear();
        m_bestResourceByLowerName.clear();
        m_packKey = manifest.key.isEmpty() ? kDefaultKey : manifest.key;
        clearEntryRawCache();
        if (!ensureSessionDirReady()) {
            if (error) *error = QString::fromUtf8(u8"无法创建低内存会话目录");
            return false;
        }
        const QString pakZipWorkDir = QDir(m_sessionDir).absoluteFilePath(QStringLiteral("pak_work_zip"));
        const QString encryptedEntryDir = QDir(m_sessionDir).absoluteFilePath(QStringLiteral("pak_entry_cache"));
        QDir().mkpath(pakZipWorkDir);
        QDir().mkpath(encryptedEntryDir);

        for (const QString &pkgName : manifest.packages) {
            QFileInfo fi(pkgName);
            const QString pakPath = fi.isAbsolute() ? fi.absoluteFilePath() : QDir(m_packsDir).absoluteFilePath(pkgName);
            if (!QFile::exists(pakPath)) continue;

            PackageInfo p;
            p.pakPath = pakPath;
            const QString pakBase = QFileInfo(pakPath).completeBaseName().toLower();
            p.packageFileNameLower = QFileInfo(pakPath).fileName().toLower();
            const QString kind = manifest.kindsByPackageLower.value(p.packageFileNameLower).toLower();
            p.packageKindLower = kind;
            p.isEnglishPack = (kind == QLatin1String("concat_eng")) || pakBase.contains(QStringLiteral("eng"));
            p.isTemplatePack = (kind == QLatin1String("template")) || pakBase.contains(QStringLiteral("template"));
            p.isPromptPack = (kind == QLatin1String("prompt"));
            if (!p.isPromptPack) {
                p.isPromptPack = pakBase.contains(QStringLiteral("prompt")) ||
                                 pakBase.contains(QStringLiteral("tip")) ||
                                 pakBase.contains(QString::fromUtf8(u8"提示")) ||
                                 pakBase.contains(QString::fromUtf8(u8"发车通知"));
            }

            const int packageIndex = m_packages.size();
            m_packages.push_back(p);
            const QHash<QString, QString> aliasMap =
                manifest.aliasesByPackageLower.value(m_packages[packageIndex].packageFileNameLower);

            int numericPromptCount = 0;
            for (int k = 1; k <= 6; ++k) {
                const QString nameWav = QStringLiteral("%1.wav").arg(k);
                const QString nameMp3 = QStringLiteral("%1.mp3").arg(k);
                for (auto it = aliasMap.begin(); it != aliasMap.end(); ++it) {
                    const QString v = it.value().toLower();
                    if (v == nameWav || v == nameMp3) {
                        ++numericPromptCount;
                        break;
                    }
                }
            }
            if (numericPromptCount >= 3) m_packages[packageIndex].isPromptPack = true;

            QString err;
            const QString zipPath = QDir(pakZipWorkDir).absoluteFilePath(
                QStringLiteral("%1_%2.work.zip").arg(packageIndex, 3, 10, QChar('0')).arg(QFileInfo(pakPath).completeBaseName()));
            if (!decryptPakToZipFile(pakPath, m_packKey, zipPath, &err)) {
                if (error) *error = err;
                return false;
            }

            QFile zipFile(zipPath);
            if (!zipFile.open(QIODevice::ReadOnly)) {
                QFile::remove(zipPath);
                if (error) *error = QString::fromUtf8(u8"Zip 打开失败：%1").arg(zipPath);
                return false;
            }
            QZipReader zip(&zipFile);
            if (zip.status() != QZipReader::NoError) {
                zipFile.close();
                QFile::remove(zipPath);
                if (error) *error = QString::fromUtf8(u8"Zip 打开失败：%1").arg(m_packages[packageIndex].pakPath);
                return false;
            }

            const QString packEntryDir = QDir(encryptedEntryDir).absoluteFilePath(
                QStringLiteral("pkg_%1").arg(packageIndex, 3, 10, QChar('0')));
            QDir().mkpath(packEntryDir);

            for (const QZipReader::FileInfo &fiInZip : zip.fileInfoList()) {
                if (fiInZip.isDir) continue;
                const QString aliasFile = QFileInfo(fiInZip.filePath).fileName();
                const QString aliasKey = aliasFile.toLower();
                QString logicalFile = aliasMap.value(aliasKey);
                if (logicalFile.isEmpty()) logicalFile = aliasFile;
                QFileInfo logicalInfo(logicalFile);
                QString suffix = logicalInfo.suffix().toLower();
                if (!isAudioSuffix(suffix)) {
                    suffix = QFileInfo(aliasFile).suffix().toLower();
                }
                if (!isAudioSuffix(suffix)) continue;

                const QByteArray plainBytes = zip.fileData(fiInZip.filePath);
                if (plainBytes.isEmpty()) continue;

                const QByteArray hashSeed =
                    m_packages[packageIndex].packageFileNameLower.toUtf8() + QByteArrayLiteral("|") +
                    fiInZip.filePath.toUtf8() + QByteArrayLiteral("|") + logicalFile.toUtf8();
                const QString encryptedName =
                    QString::fromLatin1(QCryptographicHash::hash(hashSeed, QCryptographicHash::Sha1).toHex().left(24)) +
                    QStringLiteral(".benc");
                const QString encryptedPath = QDir(packEntryDir).absoluteFilePath(encryptedName);
                QString encErr;
                if (!writeEncryptedBlob(plainBytes, encryptedPath, m_sessionBlobKey, &encErr)) {
                    zipFile.close();
                    QFile::remove(zipPath);
                    if (error) *error = QString::fromUtf8(u8"pak 条目加密缓存失败：%1").arg(encErr);
                    return false;
                }

                SourceEntry s;
                s.packageIndex = packageIndex;
                s.zipEntryPath = fiInZip.filePath;
                s.zipEntryPathLower = fiInZip.filePath.toLower();
                s.encryptedBlobPath = encryptedPath;
                s.baseFileName = logicalInfo.fileName();
                if (s.baseFileName.isEmpty()) s.baseFileName = aliasFile;
                s.stem = QFileInfo(s.baseFileName).completeBaseName().trimmed();
                s.suffix = suffix;
                s.isEnglish = m_packages[packageIndex].isEnglishPack;
                s.isTemplate =
                    m_packages[packageIndex].isTemplatePack || s.zipEntryPathLower.startsWith(QStringLiteral("template/"));
                const QString logicalLower = logicalFile.toLower();
                s.isPrompt = m_packages[packageIndex].isPromptPack ||
                             logicalLower.contains(QString::fromUtf8(u8"提示")) ||
                             logicalLower.contains(QString::fromUtf8(u8"转弯")) ||
                             logicalLower.contains(QString::fromUtf8(u8"让座")) ||
                             logicalLower.contains(QString::fromUtf8(u8"切换线路")) ||
                             logicalLower.contains(QString::fromUtf8(u8"进入公交线路选择模式")) ||
                             logicalLower.contains(QStringLiteral("prompt")) ||
                             logicalLower.contains(QStringLiteral("tip"));
                const int idx = m_sources.size();
                m_sources.push_back(s);
                m_indicesByLowerFileName.insert(s.baseFileName.toLower(), idx);
                m_indicesByLowerStem.insert(s.stem.toLower(), idx);

                const QString lowerName = s.baseFileName.toLower();
                if (!m_bestResourceByLowerPath.contains(s.zipEntryPathLower) ||
                    resourceSourceScore(s) > resourceSourceScore(m_sources[m_bestResourceByLowerPath.value(s.zipEntryPathLower)])) {
                    m_bestResourceByLowerPath.insert(s.zipEntryPathLower, idx);
                }
                if (!m_bestResourceByLowerName.contains(lowerName) ||
                    resourceSourceScore(s) > resourceSourceScore(m_sources[m_bestResourceByLowerName.value(lowerName)])) {
                    m_bestResourceByLowerName.insert(lowerName, idx);
                }
            }

            zipFile.close();
            QFile::remove(zipPath);
        }

        if (m_packages.isEmpty()) {
            if (error) *error = QString::fromUtf8(u8"未加载到任何 pak 文件");
            return false;
        }
        if (m_sources.isEmpty()) {
            if (error) *error = QString::fromUtf8(u8"pak 中没有可用音频");
            return false;
        }
        return true;
    }

    void addSourceEntry(const QString &fullPath, const QString &logicalPath, bool isEnglish, bool isTemplate, bool isPrompt) {
        const QFileInfo fileInfo(fullPath);
        const QString suffix = fileInfo.suffix().toLower();
        if (!isAudioSuffix(suffix)) return;

        SourceEntry s;
        s.packageIndex = -1;
        s.diskPath = fullPath;
        s.zipEntryPath = logicalPath;
        s.zipEntryPathLower = logicalPath.toLower();
        s.baseFileName = fileInfo.fileName();
        s.stem = fileInfo.completeBaseName().trimmed();
        s.suffix = suffix;
        s.isEnglish = isEnglish;
        s.isTemplate = isTemplate;
        s.isPrompt = isPrompt;

        const int idx = m_sources.size();
        m_sources.push_back(s);
        m_indicesByLowerFileName.insert(s.baseFileName.toLower(), idx);
        m_indicesByLowerStem.insert(s.stem.toLower(), idx);

        const QString lowerName = s.baseFileName.toLower();
        if (!m_bestResourceByLowerPath.contains(s.zipEntryPathLower) ||
            resourceSourceScore(s) > resourceSourceScore(m_sources[m_bestResourceByLowerPath.value(s.zipEntryPathLower)])) {
            m_bestResourceByLowerPath.insert(s.zipEntryPathLower, idx);
        }
        if (!m_bestResourceByLowerName.contains(lowerName) ||
            resourceSourceScore(s) > resourceSourceScore(m_sources[m_bestResourceByLowerName.value(lowerName)])) {
            m_bestResourceByLowerName.insert(lowerName, idx);
        }
    }

    bool loadPlainSourcesForSession(QString *error) {
        m_packages.clear();
        m_sources.clear();
        m_indicesByLowerFileName.clear();
        m_indicesByLowerStem.clear();
        m_bestResourceByLowerPath.clear();
        m_bestResourceByLowerName.clear();

        auto scanDir = [&](const QString &dirPath, const QString &prefix, bool isEnglish, bool isTemplate, bool isPrompt) {
            const QDir rootDir(dirPath);
            if (!rootDir.exists()) return;
            QDirIterator it(dirPath, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString fullPath = it.next();
                const QString relative = rootDir.relativeFilePath(fullPath).replace('\\', '/');
                const QString logicalPath = prefix.isEmpty() ? relative : (prefix + QLatin1Char('/') + relative);
                addSourceEntry(fullPath, logicalPath, isEnglish, isTemplate, isPrompt);
            }
        };

        scanDir(m_concatDir, QStringLiteral("00concat"), false, false, false);
        scanDir(m_concatEngDir, QStringLiteral("00concatEng"), true, false, false);
        scanDir(m_templateDir, QStringLiteral("template"), false, true, false);
        if (!m_templateLegacyDir.isEmpty() &&
            QDir::cleanPath(m_templateLegacyDir).toLower() != QDir::cleanPath(m_templateDir).toLower()) {
            scanDir(m_templateLegacyDir, QStringLiteral("template"), false, true, false);
        }
        scanDir(m_promptDir, QString::fromUtf8(u8"提示"), false, false, true);

        const QString altPrompt = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("../tips"));
        if (QDir(altPrompt).exists()) {
            scanDir(altPrompt, QStringLiteral("tips"), false, false, true);
        }

        if (m_sources.isEmpty()) {
            if (error) *error = QString::fromUtf8(u8"未找到音频源（../00concat ../00concatEng ../template ../提示）");
            return false;
        }
        return true;
    }

    bool loadSourcesForSession(QString *error) {
        const QDir packsDir(m_packsDir);
        const bool hasPak = !packsDir.entryList(QStringList() << QStringLiteral("*.pak"), QDir::Files).isEmpty();
        if (hasPak) {
            const PackManifest manifest = loadPackManifest();
            QString packErr;
            if (loadPackagesForSession(manifest, &packErr)) {
                return true;
            }
            // Fallback to development plain directories when pak loading fails.
        }
        return loadPlainSourcesForSession(error);
    }

    QByteArray sourceBytes(const SourceEntry &s, QString *error) {
        const QString key = sourceKey(s);
        if (m_entryRawCache.contains(key)) {
            return m_entryRawCache.value(key);
        }
        if (!s.encryptedBlobPath.isEmpty()) {
            QByteArray bytes;
            const QString encKey = m_sessionBlobKey.isEmpty() ? m_packKey : m_sessionBlobKey;
            if (!readEncryptedBlob(s.encryptedBlobPath, encKey, &bytes, error)) {
                return QByteArray();
            }
            cacheEntryRawBytes(key, bytes);
            return bytes;
        }
        if (!s.diskPath.isEmpty()) {
            QFile file(s.diskPath);
            if (!file.open(QIODevice::ReadOnly)) {
                if (error) *error = QStringLiteral("File missing: %1").arg(s.diskPath);
                return QByteArray();
            }
            const QByteArray bytes = file.readAll();
            cacheEntryRawBytes(key, bytes);
            return bytes;
        }
        if (error) *error = QStringLiteral("Source missing: %1").arg(s.baseFileName);
        return QByteArray();
    }

    AudioData audioForSource(const SourceEntry &s, QString *error) {
        const QString key = sourceKey(s);
        if (m_entryAudioCache.contains(key)) return m_entryAudioCache.value(key);
        const QByteArray bytes = sourceBytes(s, error);
        if (bytes.isEmpty()) return AudioData();
        const AudioData a = decodeAudioBytes(bytes, s.baseFileName);
        if (a.isValid()) m_entryAudioCache.insert(key, a);
        return a;
    }

    bool ensureSessionAndSources(QString *error) {
        if (m_sessionDir.isEmpty() && !createSessionDir()) {
            if (error) *error = QString::fromUtf8(u8"无法创建运行目录");
            return false;
        }
        if (m_sources.isEmpty()) {
            if (!loadSourcesForSession(error)) return false;
        }
        return true;
    }

    bool playSourceBytes(const QByteArray &bytes, const QString &title, const QString &nameHint, QString *error,
                         bool resetTrackSelection = true) {
        if (bytes.isEmpty()) {
            if (error) *error = QString::fromUtf8(u8"音频数据为空");
            return false;
        }
        stopAndDetachPlayer();
        if (resetTrackSelection) m_currentTrack = -1;
        m_nowPlayingLabel->setText(QString::fromUtf8(u8"当前: %1").arg(title));
        m_livePlaybackBytes = bytes;
        if (!m_livePlaybackDevice) {
            m_livePlaybackDevice = new QBuffer(this);
        } else {
            m_livePlaybackDevice->close();
        }
        m_livePlaybackDevice->setData(m_livePlaybackBytes);
        if (!m_livePlaybackDevice->open(QIODevice::ReadOnly)) {
            if (error) *error = QString::fromUtf8(u8"无法打开内存音频缓冲");
            return false;
        }
        m_player->setSourceDevice(m_livePlaybackDevice, QUrl(QStringLiteral("file:///%1").arg(nameHint)));
        m_player->play();
        updatePlayPauseButton(true);
        return true;
    }

    int selectPromptSourceByIndex(int promptIndex) const {
        int best = -1;
        int bestScore = std::numeric_limits<int>::min();
        const QString stem = QString::number(promptIndex);
        for (int i = 0; i < m_sources.size(); ++i) {
            const SourceEntry &s = m_sources[i];
            if (s.isEnglish) continue;
            int score = extensionPriority(s.suffix) * 100;
            if (s.suffix == QLatin1String("wav")) score += 500;
            if (s.isPrompt) score += 10000;
            if (s.stem == stem) score += 8000;
            else if (s.baseFileName.startsWith(stem + QLatin1Char('.'))) score += 7000;
            else continue;
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }

    int selectPromptSourceByName(const QString &name) const {
        const QString target = name.trimmed();
        if (target.isEmpty()) return -1;
        int best = -1;
        int bestScore = std::numeric_limits<int>::min();
        for (int i = 0; i < m_sources.size(); ++i) {
            const SourceEntry &s = m_sources[i];
            if (s.isEnglish) continue;
            int score = extensionPriority(s.suffix) * 100;
            if (s.isPrompt) score += 10000;
            if (s.stem == target) score += 9000;
            else if (s.stem.contains(target, Qt::CaseInsensitive) || target.contains(s.stem, Qt::CaseInsensitive)) score += 7000;
            else continue;
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }

    void playPromptByIndex(int promptIndex) {
        QString err;
        if (!ensureSessionAndSources(&err)) {
            QMessageBox::warning(this, QString::fromUtf8(u8"提示播放"), err);
            updateLoadProgress(0, QString::fromUtf8(u8"提示音加载失败：%1").arg(err), true);
            return;
        }
        const int idx = selectPromptSourceByIndex(promptIndex);
        if (idx < 0) {
            const QString msg = QString::fromUtf8(u8"未找到提示音 %1（请确认已打包“提示”资源）").arg(promptIndex);
            QMessageBox::warning(this, QString::fromUtf8(u8"提示播放"), msg);
            appendLog(msg, true);
            return;
        }
        const SourceEntry &s = m_sources[idx];
        const QByteArray bytes = sourceBytes(s, &err);
        if (!playSourceBytes(bytes, QString::fromUtf8(u8"提示%1").arg(promptIndex), s.baseFileName, &err)) {
            QMessageBox::warning(this, QString::fromUtf8(u8"提示播放"), err);
            appendLog(err, true);
            return;
        }
        appendLog(QString::fromUtf8(u8"播放提示音：%1").arg(s.baseFileName));
    }

    void playNamedPrompt(const QString &promptName) {
        QString err;
        if (!ensureSessionAndSources(&err)) {
            QMessageBox::warning(this, QString::fromUtf8(u8"提示播放"), err);
            appendLog(err, true);
            return;
        }
        const int idx = selectPromptSourceByName(promptName);
        if (idx < 0) {
            const QString msg = QString::fromUtf8(u8"未找到提示音：%1").arg(promptName);
            QMessageBox::warning(this, QString::fromUtf8(u8"提示播放"), msg);
            appendLog(msg, true);
            return;
        }
        const SourceEntry &s = m_sources[idx];
        const QByteArray bytes = sourceBytes(s, &err);
        if (!playSourceBytes(bytes, promptName, s.baseFileName, &err)) {
            QMessageBox::warning(this, QString::fromUtf8(u8"提示播放"), err);
            appendLog(err, true);
            return;
        }
        appendLog(QString::fromUtf8(u8"播放提示音：%1").arg(s.baseFileName));
    }

    bool showSynthesisOptionsDialog() {
        QDialog dlg(this);
        dlg.setWindowTitle(QString::fromUtf8(u8"合成参数"));
        dlg.setModal(true);
        dlg.resize(560, 380);

        auto *root = new QVBoxLayout(&dlg);

        auto addBoolRow = [&](const QString &title, const QString &onText, const QString &offText, bool checkedOn) {
            auto *groupBox = new QGroupBox(title, &dlg);
            auto *layout = new QHBoxLayout(groupBox);
            auto *onBtn = new QRadioButton(onText, groupBox);
            auto *offBtn = new QRadioButton(offText, groupBox);
            auto *grp = new QButtonGroup(groupBox);
            grp->addButton(onBtn, 1);
            grp->addButton(offBtn, 0);
            if (checkedOn) onBtn->setChecked(true); else offBtn->setChecked(true);
            layout->addWidget(onBtn);
            layout->addWidget(offBtn);
            layout->addStretch();
            root->addWidget(groupBox);
            return grp;
        };

        QButtonGroup *externalGrp = addBoolRow(QString::fromUtf8(u8"是否外报"), QString::fromUtf8(u8"外报"), QString::fromUtf8(u8"不外报"), m_synthOptions.externalBroadcast);
        QButtonGroup *nextStationGrp = addBoolRow(QString::fromUtf8(u8"是否下站"), QString::fromUtf8(u8"是"), QString::fromUtf8(u8"否"), m_synthOptions.includeNextStation);
        QButtonGroup *qualityGrp = addBoolRow(QString::fromUtf8(u8"高音质优先匹配"), QString::fromUtf8(u8"是"), QString::fromUtf8(u8"否"), m_synthOptions.highQuality);
        QButtonGroup *lowPassGrp = addBoolRow(QString::fromUtf8(u8"是否低音质(0-4kHz)"), QString::fromUtf8(u8"是"), QString::fromUtf8(u8"否"), m_synthOptions.lowPass);
        QButtonGroup *blindGrp = addBoolRow(QString::fromUtf8(u8"是否盲人语音"), QString::fromUtf8(u8"是"), QString::fromUtf8(u8"否"), m_synthOptions.blindMode);

        auto *missingBox = new QGroupBox(QString::fromUtf8(u8"英文缺失时"), &dlg);
        auto *missingLayout = new QHBoxLayout(missingBox);
        auto *silenceBtn = new QRadioButton(QString::fromUtf8(u8"用静音代替"), missingBox);
        auto *zhFallbackBtn = new QRadioButton(QString::fromUtf8(u8"用中文语音代替"), missingBox);
        auto *missingGrp = new QButtonGroup(missingBox);
        missingGrp->addButton(silenceBtn, 0);
        missingGrp->addButton(zhFallbackBtn, 1);
        if (m_synthOptions.missingEngUseChinese) zhFallbackBtn->setChecked(true);
        else silenceBtn->setChecked(true);
        missingLayout->addWidget(silenceBtn);
        missingLayout->addWidget(zhFallbackBtn);
        missingLayout->addStretch();
        root->addWidget(missingBox);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        buttons->button(QDialogButtonBox::Ok)->setText(QString::fromUtf8(u8"确认合成"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QString::fromUtf8(u8"取消"));
        root->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

        if (dlg.exec() != QDialog::Accepted) return false;

        m_synthOptions.externalBroadcast = (externalGrp->checkedId() == 1);
        m_synthOptions.includeNextStation = (nextStationGrp->checkedId() == 1);
        m_synthOptions.highQuality = (qualityGrp->checkedId() == 1);
        m_synthOptions.lowPass = (lowPassGrp->checkedId() == 1);
        m_synthOptions.blindMode = (blindGrp->checkedId() == 1);
        m_synthOptions.missingEngUseChinese = (missingGrp->checkedId() == 1);
        saveUserPreferences();

        appendLog(QString::fromUtf8(u8"合成参数已确认：外报=%1 下站=%2 高音质=%3 低音质=%4 盲人=%5 英文缺失=%6")
                      .arg(m_synthOptions.externalBroadcast ? QString::fromUtf8(u8"是") : QString::fromUtf8(u8"否"))
                      .arg(m_synthOptions.includeNextStation ? QString::fromUtf8(u8"是") : QString::fromUtf8(u8"否"))
                      .arg(m_synthOptions.highQuality ? QString::fromUtf8(u8"是") : QString::fromUtf8(u8"否"))
                      .arg(m_synthOptions.lowPass ? QString::fromUtf8(u8"是") : QString::fromUtf8(u8"否"))
                      .arg(m_synthOptions.blindMode ? QString::fromUtf8(u8"是") : QString::fromUtf8(u8"否"))
                      .arg(m_synthOptions.missingEngUseChinese ? QString::fromUtf8(u8"中文回退") : QString::fromUtf8(u8"静音")));
        return true;
    }

    void clearGeneratedCache() {
        stopAndDetachPlayer();
        const QString tempRoot = QDir(m_runtimeRoot).absoluteFilePath(QStringLiteral("_runtime_tmp/SecureAudioPlayer"));
        cleanupSession();

        bool ok = true;
        QDir tmpDir(tempRoot);
        if (tmpDir.exists()) {
            ok = tmpDir.removeRecursively();
        }
        QDir().mkpath(tempRoot);

        if (ok) {
            if (m_playlist) m_playlist->clear();
            if (m_loadProgress) m_loadProgress->setValue(0);
            if (m_statusLabel) m_statusLabel->setText(QString::fromUtf8(u8"缓存已清理"));
            appendLog(QString::fromUtf8(u8"已删除程序生成的缓存加密文件"));
        } else {
            QMessageBox::warning(this, QString::fromUtf8(u8"删除缓存"), QString::fromUtf8(u8"缓存目录删除失败，请检查占用"));
            appendLog(QString::fromUtf8(u8"缓存目录删除失败"), true);
        }
    }

    int stationSourceScore(const QString &station, const SourceEntry &s) const {
        int score = extensionPriority(s.suffix) * 100;
        if (s.stem == station) score += 10000;
        const QString esc = QRegularExpression::escape(station);
        if (QRegularExpression(QStringLiteral("^%1#\\d*$").arg(esc)).match(s.stem).hasMatch()) score += 9000;
        else if (QRegularExpression(QStringLiteral("^%1\\d+$").arg(esc)).match(s.stem).hasMatch()) score += 8000;
        else if (QRegularExpression(QStringLiteral("^%1(?:\\x{00B7}.+|\\(.+\\)|\\x{FF08}.+\\x{FF09})$")
                                        .arg(esc))
                     .match(s.stem)
                     .hasMatch())
            score += 7000;
        score -= s.baseFileName.size();
        return score;
    }

    int selectStationSourceIndex(const QString &stationName, bool isChinese) {
        const QString clean = stationName.trimmed();
        if (clean.isEmpty()) return -1;
        const QString cacheKey = QStringLiteral("%1|%2").arg(isChinese ? QStringLiteral("zh") : QStringLiteral("en")).arg(clean);
        if (m_stationSourceIdxCache.contains(cacheKey)) return m_stationSourceIdxCache.value(cacheKey);
        QVector<int> exactCandidates;
        int best = -1;
        int bestScore = -1;
        for (int i = 0; i < m_sources.size(); ++i) {
            const SourceEntry &s = m_sources[i];
            if (isChinese && s.isEnglish) continue;
            if (!isChinese && !s.isEnglish) continue;
            if (!isStationFileMatch(clean, s.stem)) continue;
            if (s.stem == clean) exactCandidates.push_back(i);
            const int score = stationSourceScore(clean, s);
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        if (m_synthOptions.highQuality && !exactCandidates.isEmpty()) {
            best = -1;
            bestScore = -1;
            for (int idx : exactCandidates) {
                const int score = stationSourceScore(clean, m_sources[idx]);
                if (score > bestScore) {
                    bestScore = score;
                    best = idx;
                }
            }
        }
        m_stationSourceIdxCache.insert(cacheKey, best);
        return best;
    }

    AudioData resolveStationAudio(const QString &stationName, bool isChinese, const AudioData &fallback = AudioData()) {
        const QString clean = stationName.trimmed();
        if (clean.isEmpty()) return AudioData();
        const QString cacheKey = QStringLiteral("%1|%2").arg(isChinese ? QStringLiteral("zh") : QStringLiteral("en")).arg(clean);
        if (m_stationAudioCache.contains(cacheKey)) return m_stationAudioCache.value(cacheKey);
        const int idx = selectStationSourceIndex(clean, isChinese);
        AudioData out;
        if (idx >= 0) {
            QString err;
            out = audioForSource(m_sources[idx], &err);
        }
        if (!isChinese && !out.isValid()) {
            if (m_synthOptions.missingEngUseChinese && fallback.isValid()) {
                out = fallback;
            } else {
                // 选择“静音代替”时固定为 0.1 秒静音，避免长静音或帧错位导致后续噪音。
                out = createSilentAudioMs(100, fallback.format);
            }
        }
        m_stationAudioCache.insert(cacheKey, out);
        return out;
    }

    AudioData resolveResourceAudio(const QString &fileName) {
        QString normalized = fileName;
        normalized.replace('\\', '/');
        normalized = normalized.toLower().trimmed();
        const QString lower = QFileInfo(normalized).fileName().toLower();
        if (lower.isEmpty()) return AudioData();
        const QString cacheKey = normalized.isEmpty() ? lower : normalized;
        if (m_resourceAudioCache.contains(cacheKey)) return m_resourceAudioCache.value(cacheKey);

        QStringList pathCandidates;
        pathCandidates << normalized;
        if (normalized.startsWith(QStringLiteral("template/"))) {
            pathCandidates << normalized.mid(QStringLiteral("template/").size());
        }
        pathCandidates.removeDuplicates();

        int best = -1;
        int bestScore = -1;
        for (const QString &candidate : pathCandidates) {
            if (!m_bestResourceByLowerPath.contains(candidate)) continue;
            const int idx = m_bestResourceByLowerPath.value(candidate);
            const int score = resourceSourceScore(m_sources[idx]) + 1000; // exact/near-exact path match first
            if (score > bestScore) {
                best = idx;
                bestScore = score;
            }
        }
        if (best < 0 && m_bestResourceByLowerName.contains(lower)) {
            best = m_bestResourceByLowerName.value(lower);
        }

        AudioData out;
        if (best >= 0) {
            QString err;
            out = audioForSource(m_sources[best], &err);
        }
        m_resourceAudioCache.insert(cacheKey, out);
        return out;
    }

    RuntimeTemplate materializeTemplate(const TemplateConfig &cfg, QStringList *missingResourceKeys) {
        RuntimeTemplate rt;
        rt.name = cfg.name;
        rt.hasEnglish = cfg.hasEnglish;
        rt.sequences = cfg.sequences;
        rt.resources.clear();
        if (missingResourceKeys) missingResourceKeys->clear();

        for (auto it = cfg.resources.begin(); it != cfg.resources.end(); ++it) {
            const QString key = it.key();
            const QString fileName = it.value();
            const AudioData a = resolveResourceAudio(fileName);
            if (a.isValid()) {
                rt.resources.insert(key, a);
            } else if (missingResourceKeys) {
                missingResourceKeys->append(key);
            }
        }
        return rt;
    }

    AudioData buildSequenceAudio(const QString &seqType, const RuntimeTemplate &tpl, const SynthesisContext &ctx) {
        if (!tpl.sequences.contains(seqType)) return AudioData();
        const QStringList seq = tpl.sequences.value(seqType);
        QList<AudioData> parts;
        for (const QString &token : seq) {
            if (token.startsWith(QLatin1Char('$'))) {
                if (token == QLatin1String("$LINE_NAME") || token == QLatin1String("$LINE")) parts << ctx.lineAudioChn;
                else if (token == QLatin1String("$LINE_NAME_EN") || token == QLatin1String("$LINE_EN")) parts << ctx.lineAudioEng;
                else if (token == QLatin1String("$CURRENT_STATION")) parts << ctx.currentStationChn;
                else if (token == QLatin1String("$CURRENT_STATION_EN")) parts << ctx.currentStationEng;
                else if (token == QLatin1String("$NEXT_STATION")) parts << ctx.nextStationChn;
                else if (token == QLatin1String("$NEXT_STATION_EN")) parts << ctx.nextStationEng;
                else if (token == QLatin1String("$TERMINAL") || token == QLatin1String("$TERMINAL_STATION")) parts << ctx.terminalStationChn;
                else if (token == QLatin1String("$TERMINAL_EN")) parts << ctx.terminalStationEng;
            } else {
                if (tpl.resources.contains(token)) parts << tpl.resources.value(token);
            }
        }
        return combineAudio(parts);
    }

    AudioData ensureMonoAudio(const AudioData &audio) const {
        if (!audio.isValid()) return audio;
        if (audio.format.channelCount() <= 1) return audio;
        if (audio.format.sampleFormat() != QAudioFormat::Int16) return audio;
        const int channels = audio.format.channelCount();
        const int bytesPerSample = audio.format.bytesPerSample();
        const int bytesPerFrame = audio.format.bytesPerFrame();
        if (channels <= 1 || bytesPerSample != static_cast<int>(sizeof(qint16)) || bytesPerFrame <= 0) return audio;

        const int frameCount = audio.pcmData.size() / bytesPerFrame;
        if (frameCount <= 0) return audio;

        AudioData out;
        out.format = audio.format;
        out.format.setChannelCount(1);
        out.pcmData.resize(frameCount * static_cast<int>(sizeof(qint16)));

        const qint16 *src = reinterpret_cast<const qint16*>(audio.pcmData.constData());
        qint16 *dst = reinterpret_cast<qint16*>(out.pcmData.data());
        for (int i = 0; i < frameCount; ++i) {
            int sum = 0;
            for (int c = 0; c < channels; ++c) {
                sum += src[i * channels + c];
            }
            dst[i] = static_cast<qint16>(sum / channels);
        }
        const int bps = out.format.bytesPerFrame() * out.format.sampleRate();
        if (bps > 0) {
            out.durationMs = (out.pcmData.size() * 1000LL) / bps;
        } else {
            out.durationMs = audio.durationMs;
        }
        return out;
    }

    AudioData applyLowPassAudio(const AudioData &audio) const {
        if (!audio.isValid()) return audio;
        if (audio.format.sampleFormat() != QAudioFormat::Int16 || audio.format.channelCount() != 1) return audio;
        const int sampleRate = audio.format.sampleRate();
        if (sampleRate <= 0) return audio;

        AudioData out = audio;
        const int samples = out.pcmData.size() / static_cast<int>(sizeof(qint16));
        if (samples <= 1) return out;

        qint16 *pcm = reinterpret_cast<qint16*>(out.pcmData.data());
        const double cutoffHz = 4000.0;
        const double dt = 1.0 / static_cast<double>(sampleRate);
        const double rc = 1.0 / (2.0 * 3.14159265358979323846 * cutoffHz);
        const double alpha = dt / (rc + dt);

        double y = static_cast<double>(pcm[0]);
        for (int i = 1; i < samples; ++i) {
            const double x = static_cast<double>(pcm[i]);
            y = y + alpha * (x - y);
            const double clamped = qBound(-32768.0, y, 32767.0);
            pcm[i] = static_cast<qint16>(clamped);
        }
        return out;
    }

    AudioData applyBlindAudioProcessing(const AudioData &audio, const AudioData &lineChn, const AudioData &terminalChn) {
        if (!m_synthOptions.blindMode) return audio;
        if (!audio.isValid()) return audio;
        if (QRandomGenerator::global()->generateDouble() < 0.4) return audio;

        const int bytesPerFrame = qMax(1, audio.format.bytesPerFrame());
        const double cutRatio = 0.25 + QRandomGenerator::global()->generateDouble() * 0.55;
        qint64 cutPos = static_cast<qint64>(audio.pcmData.size() * cutRatio);
        cutPos -= (cutPos % bytesPerFrame);
        cutPos = qBound<qint64>(bytesPerFrame, cutPos, audio.pcmData.size());

        AudioData processed;
        processed.format = audio.format;
        processed.pcmData = audio.pcmData.left(cutPos);

        AudioData kaiwang = resolveResourceAudio(QString::fromUtf8(u8"开往.mp3"));
        if (!kaiwang.isValid()) kaiwang = resolveResourceAudio(QString::fromUtf8(u8"开往原.mp3"));
        AudioData repeatBase = combineAudio({lineChn, kaiwang, terminalChn});
        if (repeatBase.isValid()) {
            for (int i = 0; i < 3; ++i) {
                processed.pcmData.append(repeatBase.pcmData);
            }
        }
        const int bps = processed.format.bytesPerFrame() * processed.format.sampleRate();
        if (bps > 0) processed.durationMs = (processed.pcmData.size() * 1000LL) / bps;
        return processed;
    }

    bool appendSynthTrack(const QString &title, const QString &seqType, const RuntimeTemplate &tpl,
                          const SynthesisContext &ctx, int index) {
        AudioData audio = buildSequenceAudio(seqType, tpl, ctx);
        audio = ensureMonoAudio(audio);
        if (!audio.isValid()) return false;
        const bool isMainStop = (seqType == QLatin1String("enter_station") ||
                                 seqType == QLatin1String("next_station") ||
                                 seqType == QLatin1String("terminal_station"));
        if (isMainStop) {
            audio = applyBlindAudioProcessing(audio, ctx.lineAudioChn, ctx.terminalStationChn);
        }
        audio = ensureMonoAudio(audio);
        if (m_synthOptions.lowPass) {
            audio = applyLowPassAudio(audio);
        }
        audio = ensureMonoAudio(audio);
        if (!audio.isValid()) return false;
        const QByteArray wavBytes = audioToWavBytes(audio);
        if (wavBytes.isEmpty()) return false;
        const QByteArray token = QCryptographicHash::hash(
            QStringLiteral("%1|%2|%3").arg(title, seqType).arg(index).toUtf8(),
            QCryptographicHash::Sha1);
        const QString outPath = QDir(m_sessionDir).absoluteFilePath(
            QStringLiteral("synth/%1_%2.busa")
                .arg(index, 3, 10, QChar('0'))
                .arg(QString::fromLatin1(token.toHex().left(10))));
        QDir().mkpath(QFileInfo(outPath).path());
        QString err;
        if (!writeEncryptedBlob(wavBytes, outPath, m_packKey, &err)) return false;
        TrackItem t;
        t.title = title;
        t.encryptedAudioPath = outPath;
        m_tracks.push_back(t);
        return true;
    }

    bool readStationsByLine(const QString &linePath, QStringList *out, QString *error) const {
        QStringList stations;
        if (linePath.endsWith(QStringLiteral(".xlsx"), Qt::CaseInsensitive)) stations = readXlsxStations(linePath);
        else if (linePath.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive)) stations = readTxtStations(linePath);
        if (stations.isEmpty()) {
            if (error) *error = QString::fromUtf8(u8"线路中未读取到站点：%1").arg(linePath);
            return false;
        }
        *out = stations;
        return true;
    }

    bool buildSynthesisPlaylist(const RuntimeTemplate &tpl, const QString &linePath,
                                const QStringList &stations, QString *error,
                                const std::function<void(int, int, const QString&)> &progressCb = {}) {
        m_tracks.clear();
        if (stations.isEmpty()) {
            if (error) *error = QString::fromUtf8(u8"站点列表为空");
            return false;
        }
        const QStringList requiredSequences = {
            QStringLiteral("start_station"),
            QStringLiteral("enter_station"),
            QStringLiteral("terminal_station")
        };
        for (const QString &seqName : requiredSequences) {
            if (!tpl.sequences.contains(seqName)) {
                if (error) *error = QString::fromUtf8(u8"模板缺少必须序列：%1").arg(seqName);
                return false;
            }
        }

        const QString routeId = routeIdFromLineFile(linePath);
        const AudioData lineChn = resolveStationAudio(routeId, true);
        const bool enableEnglish = tpl.hasEnglish && m_synthOptions.externalBroadcast;
        const AudioData lineEng = enableEnglish ? resolveStationAudio(routeId, false, lineChn) : AudioData();
        const AudioData terminalChn = resolveStationAudio(stations.last(), true);
        const AudioData terminalEng = enableEnglish ? resolveStationAudio(stations.last(), false, terminalChn) : AudioData();
        const bool nextFlag = m_synthOptions.includeNextStation &&
                              tpl.sequences.contains(QStringLiteral("next_station"));

        int trackCounter = 1;
        int doneCount = 0;
        const int totalTracksEstimate = nextFlag ? (2 * stations.size() - 1) : stations.size();
        auto reportProgress = [&](const QString &title) {
            if (progressCb) progressCb(doneCount, totalTracksEstimate, title);
        };
        auto buildCtx = [&](int currentIdx, int nextIdx) {
            SynthesisContext ctx;
            ctx.lineAudioChn = lineChn;
            ctx.lineAudioEng = lineEng;
            ctx.terminalStationChn = terminalChn;
            ctx.terminalStationEng = terminalEng;
            if (currentIdx >= 0 && currentIdx < stations.size()) {
                ctx.currentStationChn = resolveStationAudio(stations[currentIdx], true);
                if (enableEnglish) ctx.currentStationEng = resolveStationAudio(stations[currentIdx], false, ctx.currentStationChn);
            }
            if (nextIdx >= 0 && nextIdx < stations.size()) {
                ctx.nextStationChn = resolveStationAudio(stations[nextIdx], true);
                if (enableEnglish) ctx.nextStationEng = resolveStationAudio(stations[nextIdx], false, ctx.nextStationChn);
            }
            return ctx;
        };

        auto appendWithProgress = [&](const QString &title, const QString &seqType, const SynthesisContext &ctx) {
            if (appendSynthTrack(title, seqType, tpl, ctx, trackCounter++)) {
                ++doneCount;
                reportProgress(title);
            }
        };

        appendWithProgress(QStringLiteral("%1 %2").arg(1, 2, 10, QChar('0')).arg(stations.first()),
                           QStringLiteral("start_station"), buildCtx(0, stations.size() > 1 ? 1 : -1));

        if (nextFlag && stations.size() > 1) {
            appendWithProgress(QStringLiteral("%1-\u4E0B %2").arg(1, 2, 10, QChar('0')).arg(stations[1]),
                               QStringLiteral("next_station"), buildCtx(0, 1));
        }

        for (int i = 1; i < stations.size() - 1; ++i) {
            appendWithProgress(QStringLiteral("%1 %2").arg(i + 1, 2, 10, QChar('0')).arg(stations[i]),
                               QStringLiteral("enter_station"), buildCtx(i, i + 1));
            if (nextFlag) {
                appendWithProgress(QStringLiteral("%1-\u4E0B %2").arg(i + 1, 2, 10, QChar('0')).arg(stations[i + 1]),
                                   QStringLiteral("next_station"), buildCtx(i, i + 1));
            }
        }

        if (stations.size() > 1) {
            const int idx = stations.size() - 1;
            appendWithProgress(QStringLiteral("%1 %2").arg(stations.size(), 2, 10, QChar('0')).arg(stations.last()),
                               QStringLiteral("terminal_station"), buildCtx(idx, -1));
        }

        if (m_tracks.isEmpty()) {
            if (error) *error = QString::fromUtf8(u8"synthesis result empty check（未合成出可播放语音，请检查资源/模板）");
            return false;
        }
        return true;
    }

    bool loadSelectedConfigAndLine() {
        if (m_cfgCombo->currentIndex() < 0 || m_cfgCombo->currentIndex() >= m_templates.size()) {
            QMessageBox::warning(this, QString::fromUtf8(u8"提示"), QString::fromUtf8(u8"请先选择配置"));
            return false;
        }
        if (m_lineCombo->currentIndex() < 0) {
            QMessageBox::warning(this, QString::fromUtf8(u8"提示"), QString::fromUtf8(u8"请先选择线路"));
            return false;
        }
        if (!showSynthesisOptionsDialog()) {
            appendLog(QString::fromUtf8(u8"已取消本次加载"));
            return false;
        }
        updateLoadProgress(3, QString::fromUtf8(u8"开始加载配置与线路..."));
        if (!createSessionDir()) {
            QMessageBox::critical(this, QString::fromUtf8(u8"错误"), QString::fromUtf8(u8"无法创建运行目录"));
            return false;
        }

        const TemplateConfig cfg = m_templates[m_cfgCombo->currentIndex()];
        QString linePath = m_lineCombo->currentData().toString();
        if (linePath.isEmpty()) {
            const QString selectedBase = m_lineCombo->currentText().trimmed();
            const QDir lineDir(m_linesDir);
            const QStringList allLineFiles = lineDir.entryList(QStringList() << QStringLiteral("*.xlsx") << QStringLiteral("*.txt"),
                                                               QDir::Files, QDir::Name);
            for (const QString &name : allLineFiles) {
                if (QFileInfo(name).completeBaseName().compare(selectedBase, Qt::CaseInsensitive) == 0) {
                    linePath = lineDir.absoluteFilePath(name);
                    break;
                }
            }
            if (linePath.isEmpty()) linePath = QDir(m_linesDir).absoluteFilePath(m_lineCombo->currentText());
        }
        updateLoadProgress(12, QString::fromUtf8(u8"运行目录准备完成"));

        QString err;
        if (!ensureSessionAndSources(&err)) {
            QMessageBox::critical(this, QString::fromUtf8(u8"错误"), err);
            updateLoadProgress(100, QString::fromUtf8(u8"加载失败：%1").arg(err), true);
            return false;
        }
        updateLoadProgress(35, QString::fromUtf8(u8"音频资源已加载，开始读取线路站点"));

        QStringList stations;
        if (!readStationsByLine(linePath, &stations, &err)) {
            QMessageBox::critical(this, QString::fromUtf8(u8"错误"), err);
            updateLoadProgress(100, QString::fromUtf8(u8"加载失败：%1").arg(err), true);
            return false;
        }
        updateLoadProgress(52, QString::fromUtf8(u8"读取到 %1 个站点，开始解析模板资源").arg(stations.size()));

        QStringList missingResourceKeys;
        const RuntimeTemplate runtimeTemplate = materializeTemplate(cfg, &missingResourceKeys);
        updateLoadProgress(66, QString::fromUtf8(u8"模板解析完成，开始即时合成语音"));

        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = buildSynthesisPlaylist(
            runtimeTemplate, linePath, stations, &err,
            [this](int done, int total, const QString &title) {
                const int percent = 66 + (total > 0 ? (done * 28 / total) : 0);
                updateLoadProgress(percent, QString::fromUtf8(u8"正在合成：%1").arg(title));
            });
        QApplication::restoreOverrideCursor();
        if (!ok) {
            QMessageBox::critical(this, QString::fromUtf8(u8"错误"), err);
            updateLoadProgress(100, QString::fromUtf8(u8"合成失败：%1").arg(err), true);
            return false;
        }

        m_playlist->clear();
        for (const TrackItem &t : std::as_const(m_tracks)) m_playlist->addItem(t.title);
        if (!m_tracks.isEmpty()) m_playlist->setCurrentRow(0);
        if (!missingResourceKeys.isEmpty()) {
            m_statusLabel->setText(QString::fromUtf8(u8"已生成 %1 条，缺失模板资源 %2 项")
                                       .arg(m_tracks.size())
                                       .arg(missingResourceKeys.size()));
            appendLog(QString::fromUtf8(u8"模板有缺失资源，已尽量完成可合成部分"), true);
        } else {
            m_statusLabel->setText(QString::fromUtf8(u8"已生成 %1 条，缓存已启用").arg(m_tracks.size()));
        }
        saveUserPreferences();
        updateLoadProgress(100, QString::fromUtf8(u8"加载完成，可直接播放"));
        return true;
    }

    void stopAndDetachPlayer() {
        m_player->stop();
        m_player->setSource(QUrl());
        m_player->setSourceDevice(nullptr);
        if (m_livePlaybackDevice) {
            m_livePlaybackDevice->close();
        }
        m_livePlaybackBytes.clear();
        updatePlayPauseButton(false);
    }

    void playTrack(int index) {
        if (index < 0 || index >= m_tracks.size()) return;
        const TrackItem &t = m_tracks[index];
        QString err;
        if (t.sourceIndex >= 0) {
            if (t.sourceIndex < 0 || t.sourceIndex >= m_sources.size()) return;
            const SourceEntry &s = m_sources[t.sourceIndex];
            const QByteArray bytes = sourceBytes(s, &err);
            if (bytes.isEmpty()) {
                appendLog(err, true);
                QMessageBox::warning(this, QString::fromUtf8(u8"播放失败"),
                                     err.isEmpty() ? QString::fromUtf8(u8"音频读取失败") : err);
                return;
            }
            stopAndDetachPlayer();
            m_currentTrack = index;
            if (!playSourceBytes(bytes, t.title, s.baseFileName, &err, false)) {
                appendLog(err, true);
                QMessageBox::warning(this, QString::fromUtf8(u8"播放失败"),
                                     err.isEmpty() ? QString::fromUtf8(u8"播放失败") : err);
            }
            return;
        }
        if (!QFile::exists(t.encryptedAudioPath)) return;
        QByteArray wavBytes;
        if (!readEncryptedBlob(t.encryptedAudioPath, m_packKey, &wavBytes, &err)) {
            appendLog(err, true);
            QMessageBox::warning(this, QString::fromUtf8(u8"播放失败"), err);
            return;
        }
        stopAndDetachPlayer();
        m_currentTrack = index;
        if (!playSourceBytes(wavBytes, t.title, QStringLiteral("track.wav"), &err, false)) {
            appendLog(err, true);
            QMessageBox::warning(this, QString::fromUtf8(u8"播放失败"), err);
            return;
        }
    }

    void playNext() {
        if (m_tracks.isEmpty()) return;
        int current = m_currentTrack;
        if (current < 0) current = m_playlist->currentRow();
        if (current < 0) current = 0;
        const int next = (current + 1) % m_tracks.size();
        if (next == current) {
            playTrack(next);
        } else {
            m_playlist->setCurrentRow(next);
        }
    }

    void playPrev() {
        if (m_tracks.isEmpty()) return;
        int current = m_currentTrack;
        if (current < 0) current = m_playlist->currentRow();
        if (current < 0) current = 0;
        const int prev = (current - 1 + m_tracks.size()) % m_tracks.size();
        if (prev == current) {
            playTrack(prev);
        } else {
            m_playlist->setCurrentRow(prev);
        }
    }

    void handleEndOfMedia() {
        if (m_tracks.isEmpty() || m_currentTrack < 0 || m_currentTrack >= m_tracks.size()) {
            updatePlayPauseButton(false);
            return;
        }
        switch (m_playMode) {
        case PlaybackMode::SinglePlay:
            updatePlayPauseButton(false);
            break;
        case PlaybackMode::SingleLoop:
            playTrack(m_currentTrack);
            break;
        case PlaybackMode::Random: {
            const int idx = randomTrackIndex(m_currentTrack);
            if (idx >= 0) {
                if (idx == m_currentTrack) playTrack(idx);
                else m_playlist->setCurrentRow(idx);
            }
            break;
        }
        case PlaybackMode::ListLoop: {
            const int idx = (m_currentTrack + 1) % m_tracks.size();
            if (idx == m_currentTrack) playTrack(idx);
            else m_playlist->setCurrentRow(idx);
            break;
        }
        case PlaybackMode::Sequential:
        default:
            if (m_currentTrack + 1 < m_tracks.size()) {
                m_playlist->setCurrentRow(m_currentTrack + 1);
            } else {
                updatePlayPauseButton(false);
            }
            break;
        }
    }

    void togglePlayPause() {
        if (m_tracks.isEmpty()) return;
        if (m_player->source().isEmpty() &&
            (m_livePlaybackBytes.isEmpty() || !m_livePlaybackDevice || !m_livePlaybackDevice->isOpen())) {
            m_playlist->setCurrentRow(m_currentTrack >= 0 ? m_currentTrack : 0);
            return;
        }
        if (m_player->playbackState() == QMediaPlayer::PlayingState) {
            m_player->pause();
            updatePlayPauseButton(false);
        } else {
            m_player->play();
            updatePlayPauseButton(true);
        }
    }

    void cleanupSession() {
        stopAndDetachPlayer();
        m_nowPlayingLabel->setText(QString::fromUtf8(u8"当前: （无）"));
        m_progress->setRange(0, 0);
        m_progress->setValue(0);
        m_timeLabel->setText(QStringLiteral("00:00 / 00:00"));
        m_currentTrack = -1;
        m_tracks.clear();
        m_sources.clear();
        m_packages.clear();
        m_indicesByLowerFileName.clear();
        m_indicesByLowerStem.clear();
        m_bestResourceByLowerPath.clear();
        m_bestResourceByLowerName.clear();
        clearEntryRawCache();
        m_entryAudioCache.clear();
        m_stationAudioCache.clear();
        m_stationSourceIdxCache.clear();
        m_resourceAudioCache.clear();
        m_sessionBlobKey.clear();
        if (!m_sessionDir.isEmpty()) {
            QDir d(m_sessionDir);
            if (d.exists()) d.removeRecursively();
            m_sessionDir.clear();
        }
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QString::fromUtf8(u8"杭州公交报站语音库"));
    app.setOrganizationName(QStringLiteral("BusAnnouncement"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));
    SecurePlayerWindow w;
    w.show();
    return app.exec();
}
