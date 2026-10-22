#include <QTest>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QBuffer>
#include <QDir>
#include <QProcess>

#include "tools/diagnostics/CrashAnalyzer.h"
#include "tools/diagnostics/ModAuditor.h"
#include "settings/INISettingsObject.h"
#include "settings/Setting.h"

class DiagnosticsTest : public QObject {
    Q_OBJECT

private slots:
    void test_CrashAnalyzer_MixinConflict()
    {
        QString log =
            "[12:34:56] [main/ERROR]: Minecraft has crashed!\n"
            "org.spongepowered.asm.mixin.transformer.throwables.MixinTransformerError: An unexpected critical error was encountered\n"
            "Caused by: org.spongepowered.asm.mixin.throwables.MixinApplyError: Mixin [sodium.mixins.core.json:client.MixinMinecraftClient] "
            "from phase [DEFAULT] in config [sodium.mixins.core.json] FAILED during APPLY\n"
            "Caused by: org.spongepowered.asm.mixin.injection.throwables.InvalidInjectionException: Critical injection failure\n";

        auto issues = CrashAnalyzer::analyze(log);
        QVERIFY(!issues.isEmpty());
        QCOMPARE(issues.first().id, QStringLiteral("mixin_failure"));
        QVERIFY(issues.first().rawEvidence.join('\n').contains("MixinMinecraftClient") ||
                issues.first().rawEvidence.join('\n').contains("sodium.mixins.core.json"));
    }

    void test_CrashAnalyzer_JavaVersionMismatch()
    {
        QString log =
            "Exception in thread \"main\" java.lang.UnsupportedClassVersionError: "
            "net/fabricmc/example/ExampleMod has been compiled by a more recent version of the Java Runtime (class file version 65.0), "
            "this version of the Java Runtime only recognizes class file versions up to 61.0\n";

        auto issues = CrashAnalyzer::analyze(log);
        QVERIFY(!issues.isEmpty());
        QCOMPARE(issues.first().id, QStringLiteral("java_version_incompatible"));
        QVERIFY(issues.first().recommendation.contains("Java"));
    }

    void test_CrashAnalyzer_OutOfMemory()
    {
        QString log =
            "Exception in thread \"Client thread\" java.lang.OutOfMemoryError: Java heap space\n"
            "    at java.base/java.util.Arrays.copyOf(Arrays.java:3537)\n";

        auto issues = CrashAnalyzer::analyze(log);
        QVERIFY(!issues.isEmpty());
        QCOMPARE(issues.first().id, QStringLiteral("out_of_memory"));
    }

    void test_CrashAnalyzer_DriverCrash()
    {
        QString log =
            "# A fatal error has been detected by the Java Runtime Environment:\n"
            "#\n"
            "#  EXCEPTION_ACCESS_VIOLATION (0xc0000005) at pc=0x00007ffd1a2b3c4d, pid=1234, tid=5678\n"
            "#\n"
            "# Problematic frame:\n"
            "# C  [nvoglv64.dll+0x9c3f21]\n";

        auto issues = CrashAnalyzer::analyze(log);
        QVERIFY(!issues.isEmpty());
        QCOMPARE(issues.first().id, QStringLiteral("graphics_failure"));
    }

    void test_CrashAnalyzer_MissingDependencies()
    {
        QString log =
            "[main/FATAL] [net.fabricmc.loader.impl.discovery.ModResolver/]: "
            "Mod resolution encountered an error!\n"
            "Incompatible mod set!\n"
            "net.fabricmc.loader.impl.FormattedException: Some of your mods are incompatible with the game or each other!\n"
            "A potential solution has been determined:\n"
            " - Install fabric-api, version 0.90.0 or later.\n";

        auto issues = CrashAnalyzer::analyze(log);
        QVERIFY(!issues.isEmpty());
        QCOMPARE(issues.first().id, QStringLiteral("missing_dependencies"));
    }

    void test_ModAuditor_CleanJar()
    {
        // Audit NewLaunch.jar from the build output
        QString jarPath = QDir::current().absoluteFilePath("jars/NewLaunch.jar");
        if (!QFile::exists(jarPath)) {
            jarPath = QDir::current().absoluteFilePath("../build/jars/NewLaunch.jar");
        }

        if (QFile::exists(jarPath)) {
            auto report = ModAuditor::auditJar(jarPath);
            QVERIFY(report.classCount > 0);
            QVERIFY(report.riskScore < 50);
            QVERIFY(report.verdict != AuditVerdict::Dangerous);
        }
    }

    void test_ModAuditor_MaliciousJar()
    {
        // Construct a synthetic class with CAFEBABE and constant pool containing a Discord webhook
        // Class file format:
        // u4 magic = 0xCAFEBABE
        // u2 minor = 0, u2 major = 52 (Java 8)
        // u2 constant_pool_count = 3
        // tag 1 = CONSTANT_Utf8 (string length + bytes)
        // tag 7 = CONSTANT_Class (index 1)
        // u2 access_flags = 0x0001 (ACC_PUBLIC)
        // u2 this_class = 2
        // u2 super_class = 0
        // u2 interfaces_count = 0
        // u2 fields_count = 0
        // u2 methods_count = 0
        // u2 attributes_count = 0

        QByteArray classBytes;
        QDataStream ds(&classBytes, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::BigEndian);

        ds << quint32(0xCAFEBABE);
        ds << quint16(0) << quint16(52); // Java 8
        ds << quint16(3); // constant pool count (entries 1 and 2)

        // Entry 1: CONSTANT_Utf8 with Discord Webhook
        const char webhookStr[] = "https://discord.com/api/webhooks/1234567890/token_secret_abc";
        quint16 strLen = sizeof(webhookStr) - 1;
        ds << quint8(1) << strLen;
        classBytes.append(webhookStr, strLen);
        ds.device()->seek(classBytes.size());

        // Entry 2: CONSTANT_Class pointing to entry 1
        ds << quint8(7) << quint16(1);

        // Class declaration
        ds << quint16(0x0001); // ACC_PUBLIC
        ds << quint16(2);      // this_class = entry 2
        ds << quint16(0);      // super_class
        ds << quint16(0);      // interfaces
        ds << quint16(0);      // fields
        ds << quint16(0);      // methods
        ds << quint16(0);      // attributes

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QString classFilePath = tempDir.filePath("Stealer.class");
        QFile classFile(classFilePath);
        QVERIFY(classFile.open(QIODevice::WriteOnly));
        classFile.write(classBytes);
        classFile.close();

        QString jarFilePath = tempDir.filePath("test_stealer.jar");
        QProcess zipProc;
        zipProc.setWorkingDirectory(tempDir.path());
        zipProc.start("zip", QStringList() << "-j" << jarFilePath << classFilePath);
        QVERIFY(zipProc.waitForFinished(5000));
        QCOMPARE(zipProc.exitCode(), 0);

        auto report = ModAuditor::auditJar(jarFilePath);
        QCOMPARE(report.classCount, 1);
        QVERIFY(!report.findings.isEmpty());
        QVERIFY(report.riskScore >= 35);
        QCOMPARE(report.verdict, AuditVerdict::Dangerous);

        bool foundWebhook = false;
        for (const auto& f : report.findings) {
            if (f.category.contains("Webhook") || f.description.contains("Discord")) {
                foundWebhook = true;
                break;
            }
        }
        QVERIFY(foundWebhook);
    }

    void test_ModAuditor_SodiumBenignCalls()
    {
        QString sodiumJar = "/home/joke/.local/share/PrismLauncher/instances/BlockFront/minecraft/mods/sodium-neoforge-0.8.13+mc1.21.1.jar";
        if (QFile::exists(sodiumJar)) {
            auto report = ModAuditor::auditJar(sodiumJar);
            QVERIFY(report.classCount > 0);
            QCOMPARE(report.riskScore, 0);
            QCOMPARE(report.verdict, AuditVerdict::Clean);
        }
    }

    void test_CrashAnalyzer_ActionableIssues()
    {
        QString log =
            "A potential solution has been determined:\n"
            " - Install fabric-api, version 0.90.0 or later.\n";

        auto issues = CrashAnalyzer::analyze(log);
        QVERIFY(!issues.isEmpty());
        QCOMPARE(issues.first().actionType, CrashActionType::DownloadMissingMod);
        QCOMPARE(issues.first().actionTarget, QStringLiteral("fabric-api"));
        QVERIFY(!issues.first().actionLabel.isEmpty());
    }

    void test_ModAuditor_Cache()
    {
        ModAuditor::clearCache();

        QString dummyPath = "/tmp/test_dummy_mod.jar";
        AuditReport report;
        report.jarPath = dummyPath;
        report.modName = "Test Mod";
        report.riskScore = 80;
        report.verdict = AuditVerdict::Dangerous;

        QVERIFY(!ModAuditor::hasCachedReport(dummyPath));

        ModAuditor::setCachedReport(dummyPath, report);
        QVERIFY(ModAuditor::hasCachedReport(dummyPath));

        auto cached = ModAuditor::getCachedReport(dummyPath);
        QCOMPARE(cached.modName, QStringLiteral("Test Mod"));
        QCOMPARE(cached.verdict, AuditVerdict::Dangerous);

        // Check key normalization with .disabled
        QVERIFY(ModAuditor::hasCachedReport(dummyPath + ".disabled"));
        auto cachedDisabled = ModAuditor::getCachedReport(dummyPath + ".disabled");
        QCOMPARE(cachedDisabled.modName, QStringLiteral("Test Mod"));

        ModAuditor::clearCache();
        QVERIFY(!ModAuditor::hasCachedReport(dummyPath));
    }

    void test_ModAuditor_VerdictIcons()
    {
        QCOMPARE(ModAuditor::verdictToIconName(AuditVerdict::Dangerous), QStringLiteral("dialog-error"));
        QCOMPARE(ModAuditor::verdictToIconName(AuditVerdict::Suspicious), QStringLiteral("dialog-warning"));
        QCOMPARE(ModAuditor::verdictToIconName(AuditVerdict::Notice), QStringLiteral("dialog-information"));
        QCOMPARE(ModAuditor::verdictToIconName(AuditVerdict::Clean), QStringLiteral("security-high"));

        QCOMPARE(ModAuditor::verdictToString(AuditVerdict::Dangerous), QStringLiteral("ОПАСНО"));
        QCOMPARE(ModAuditor::verdictToString(AuditVerdict::Suspicious), QStringLiteral("ВНИМАНИЕ"));
        QCOMPARE(ModAuditor::verdictToString(AuditVerdict::Notice), QStringLiteral("ЗАМЕЧАНИЕ"));
        QCOMPARE(ModAuditor::verdictToColor(AuditVerdict::Dangerous), QStringLiteral("#dc3545"));
        QCOMPARE(ModAuditor::verdictToColor(AuditVerdict::Suspicious), QStringLiteral("#e67e22"));
        QCOMPARE(ModAuditor::verdictToColor(AuditVerdict::Notice), QStringLiteral("#3498db"));
        QCOMPARE(ModAuditor::verdictToColor(AuditVerdict::Clean), QStringLiteral("#28a745"));
    }

    static QString buildTestJar(const QString& dirPath, const QString& classRelPath, const QStringList& utf8Strings, const QStringList& classNames = {})
    {
        QByteArray bytes;
        QDataStream ds(&bytes, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::BigEndian);

        ds << quint32(0xCAFEBABE);
        ds << quint16(0) << quint16(52);

        quint16 cpCount = 1;
        int thisClassUtf8 = cpCount++;
        int thisClassRef = cpCount++;

        QList<int> stringUtf8Idxs;
        QList<int> stringRefIdxs;
        for (int i = 0; i < utf8Strings.size(); ++i) {
            stringUtf8Idxs.append(cpCount++);
            stringRefIdxs.append(cpCount++);
        }

        QList<int> classUtf8Idxs;
        QList<int> classRefIdxs;
        for (int i = 0; i < classNames.size(); ++i) {
            classUtf8Idxs.append(cpCount++);
            classRefIdxs.append(cpCount++);
        }

        ds << cpCount;

        QByteArray thisClassUtf8Bytes = classRelPath.toUtf8();
        if (thisClassUtf8Bytes.endsWith(".class")) thisClassUtf8Bytes.chop(6);
        ds << quint8(1) << quint16(thisClassUtf8Bytes.size());
        bytes.append(thisClassUtf8Bytes);
        ds.device()->seek(bytes.size());

        ds << quint8(7) << quint16(thisClassUtf8);

        for (int i = 0; i < utf8Strings.size(); ++i) {
            QByteArray sBytes = utf8Strings[i].toUtf8();
            ds << quint8(1) << quint16(sBytes.size());
            bytes.append(sBytes);
            ds.device()->seek(bytes.size());

            ds << quint8(8) << quint16(stringUtf8Idxs[i]);
        }

        for (int i = 0; i < classNames.size(); ++i) {
            QByteArray cBytes = classNames[i].toUtf8();
            ds << quint8(1) << quint16(cBytes.size());
            bytes.append(cBytes);
            ds.device()->seek(bytes.size());

            ds << quint8(7) << quint16(classUtf8Idxs[i]);
        }

        ds << quint16(0x0001);
        ds << quint16(thisClassRef);
        ds << quint16(0);
        ds << quint16(0);
        ds << quint16(0);
        ds << quint16(0);
        ds << quint16(0);

        QString classFilePath = dirPath + "/" + classRelPath;
        QFileInfo fi(classFilePath);
        QDir().mkpath(fi.absolutePath());

        QFile file(classFilePath);
        if (!file.open(QIODevice::WriteOnly)) return {};
        file.write(bytes);
        file.close();

        QString jarPath = dirPath + "/" + fi.baseName() + ".jar";
        QProcess zipProc;
        zipProc.setWorkingDirectory(dirPath);
        zipProc.start("zip", QStringList() << "-r" << jarPath << classRelPath);
        if (!zipProc.waitForFinished(5000) || zipProc.exitCode() != 0) return {};

        return jarPath;
    }

    void test_ModAuditor_Whitelist()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        // Whitelisted OSHI class with process/runtime references
        QString oshiJar = buildTestJar(tempDir.path(), "oshi/hardware/platform/windows/WindowsCentralProcessor.class",
                                       { "wmic cpu get" }, { "java/lang/Runtime", "java/lang/ProcessBuilder" });
        QVERIFY(!oshiJar.isEmpty());
        auto reportOshi = ModAuditor::auditJar(oshiJar);
        QCOMPARE(reportOshi.riskScore, 0);
        QCOMPARE(reportOshi.verdict, AuditVerdict::Clean);

        // Whitelisted Netty class
        QString nettyJar = buildTestJar(tempDir.path(), "io/netty/channel/epoll/Native.class",
                                        {}, { "java/net/Socket", "java/lang/ProcessBuilder" });
        QVERIFY(!nettyJar.isEmpty());
        auto reportNetty = ModAuditor::auditJar(nettyJar);
        QCOMPARE(reportNetty.riskScore, 0);
        QCOMPARE(reportNetty.verdict, AuditVerdict::Clean);
    }

    void test_ModAuditor_StealerDetection()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QString stealerJar = buildTestJar(tempDir.path(), "com/mod/malware/Stealer.class",
                                          { "C:\\Users\\User\\.minecraft\\launcher_accounts.json",
                                            "discord/Local Storage/leveldb",
                                            "exodus.wallet",
                                            "Metamask" });
        QVERIFY(!stealerJar.isEmpty());
        auto report = ModAuditor::auditJar(stealerJar);
        QVERIFY(report.riskScore >= 95);
        QCOMPARE(report.verdict, AuditVerdict::Dangerous);

        bool foundStealerCat = false;
        for (const auto& f : report.findings) {
            if (f.category.contains("Кража данных") || f.category.contains("Data Theft")) {
                foundStealerCat = true;
                break;
            }
        }
        QVERIFY(foundStealerCat);
    }

    void test_ModAuditor_Exfiltration()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QString exfilJar = buildTestJar(tempDir.path(), "com/mod/Sender.class",
                                        { "https://api.telegram.org/bot123456:ABC-DEF/sendMessage",
                                          "https://transfer.sh/stolen.zip" });
        QVERIFY(!exfilJar.isEmpty());
        auto report = ModAuditor::auditJar(exfilJar);
        QVERIFY(report.riskScore >= 75);
        QCOMPARE(report.verdict, AuditVerdict::Dangerous);

        bool foundExfilCat = false;
        for (const auto& f : report.findings) {
            if (f.category.contains("Скрытая отправка данных") || f.category.contains("Data Exfiltration")) {
                foundExfilCat = true;
                break;
            }
        }
        QVERIFY(foundExfilCat);
    }

    void test_ModAuditor_Persistence()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QString persistJar = buildTestJar(tempDir.path(), "com/mod/Autostart.class",
                                          { "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                            "powershell -WindowStyle Hidden -enc JABhID0A...",
                                            ".config/autostart/minecraft_updater.desktop" });
        QVERIFY(!persistJar.isEmpty());
        auto report = ModAuditor::auditJar(persistJar);
        QVERIFY(report.riskScore >= 95);
        QCOMPARE(report.verdict, AuditVerdict::Dangerous);

        bool foundPersistCat = false;
        for (const auto& f : report.findings) {
            if (f.category.contains("Автозагрузка") || f.category.contains("System Persistence")) {
                foundPersistCat = true;
                break;
            }
        }
        QVERIFY(foundPersistCat);
    }

    void test_ModAuditor_ComboRules()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        // 1. Clean Stage 0 Downloader combo in readable code: URLClassLoader + explicit remote URL (https://)
        // Must give 40 points (Warning / Suspicious), NOT 85 or Dangerous!
        QString cleanStage0Jar = buildTestJar(tempDir.path(), "com/mod/Downloader.class",
                                              { "https://repo.example.com/payload.jar" },
                                              { "java/net/URLClassLoader" });
        QVERIFY(!cleanStage0Jar.isEmpty());
        auto reportCleanStage0 = ModAuditor::auditJar(cleanStage0Jar);
        QCOMPARE(reportCleanStage0.riskScore, 40);
        QCOMPARE(reportCleanStage0.verdict, AuditVerdict::Suspicious);

        // 2. Legal Mixin defineClass without remote network (http/https/socket)
        // Must NOT trigger Stage 0 Downloader (score 0, Clean)
        QString mixinCodeJar = buildTestJar(tempDir.path(), "com/mod/MixinTransformer.class",
                                            { "defineClass" }, { "java/net/URL" });
        QVERIFY(!mixinCodeJar.isEmpty());
        auto reportMixin = ModAuditor::auditJar(mixinCodeJar);
        QCOMPARE(reportMixin.riskScore, 0);
        QCOMPARE(reportMixin.verdict, AuditVerdict::Clean);

        // 3. Clean Network Dropper combo in readable code: ProcessBuilder + URL (Flashback pattern)
        // Must give 40 points (Warning / Suspicious), NOT Dangerous!
        QString cleanDropperJar = buildTestJar(tempDir.path(), "com/mod/Dropper.class",
                                               {}, { "java/lang/ProcessBuilder", "java/net/URL" });
        QVERIFY(!cleanDropperJar.isEmpty());
        auto reportCleanDropper = ModAuditor::auditJar(cleanDropperJar);
        QCOMPARE(reportCleanDropper.riskScore, 40);
        QCOMPARE(reportCleanDropper.verdict, AuditVerdict::Suspicious);

        // 4. Aggravated Stage 0 Downloader: obfuscated class "a.class" with URLClassLoader + https URL
        // Must escalate to 95 points (Critical / Dangerous)!
        QString aggStage0Jar = buildTestJar(tempDir.path(), "a.class",
                                            { "https://repo.example.com/payload.jar" },
                                            { "java/net/URLClassLoader" });
        QVERIFY(!aggStage0Jar.isEmpty());
        auto reportAggStage0 = ModAuditor::auditJar(aggStage0Jar);
        QVERIFY(reportAggStage0.riskScore >= 95);
        QCOMPARE(reportAggStage0.verdict, AuditVerdict::Dangerous);

        // 5. Aggravated Dropper: hidden shell command (powershell -w hidden) + ProcessBuilder + URL
        // Must escalate to 95 points (Critical / Dangerous)!
        QString aggDropperJar = buildTestJar(tempDir.path(), "com/mod/StealthDropper.class",
                                             { "powershell.exe -windowstyle hidden -enc AAAA..." },
                                             { "java/lang/ProcessBuilder", "java/net/URL" });
        QVERIFY(!aggDropperJar.isEmpty());
        auto reportAggDropper = ModAuditor::auditJar(aggDropperJar);
        QVERIFY(reportAggDropper.riskScore >= 95);
        QCOMPARE(reportAggDropper.verdict, AuditVerdict::Dangerous);

        // 6. Obfuscation combo: "ns.class" with ProcessBuilder
        QString obfJar = buildTestJar(tempDir.path(), "ns.class",
                                      {}, { "java/lang/ProcessBuilder" });
        QVERIFY(!obfJar.isEmpty());
        auto reportObf = ModAuditor::auditJar(obfJar);
        QVERIFY(reportObf.riskScore >= 40);

        bool foundObfCat = false;
        for (const auto& f : reportObf.findings) {
            if (f.category.contains("Подозрительная обфускация") || f.category.contains("Suspicious Obfuscation")) {
                foundObfCat = true;
                break;
            }
        }
        QVERIFY(foundObfCat);
    }

    void test_ModAuditor_Deduplication()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        // Create 2 classes in the same mod archive triggering the same rule (Flashback ffmpeg pattern)
        QString class1Path = tempDir.path() + "/com/mod/DownloaderA.class";
        QString class2Path = tempDir.path() + "/com/mod/DownloaderB.class";
        QFileInfo(class1Path).dir().mkpath(".");

        // Write class 1
        buildTestJar(tempDir.path(), "com/mod/DownloaderA.class", {}, { "java/lang/ProcessBuilder", "java/net/URL" });
        // Write class 2
        buildTestJar(tempDir.path(), "com/mod/DownloaderB.class", {}, { "java/lang/ProcessBuilder", "java/net/URL" });

        // Package both into one jar
        QString multiClassJar = tempDir.filePath("multi_class_mod.jar");
        QProcess zipProc;
        zipProc.setWorkingDirectory(tempDir.path());
        zipProc.start("zip", QStringList() << "-r" << multiClassJar << "com");
        QVERIFY(zipProc.waitForFinished(5000));
        QCOMPARE(zipProc.exitCode(), 0);

        auto report = ModAuditor::auditJar(multiClassJar);
        QCOMPARE(report.classCount, 2);

        // Score must NOT be summed (40 + 40 = 80); it should count the category once: 40!
        QCOMPARE(report.riskScore, 40);
        QCOMPARE(report.verdict, AuditVerdict::Suspicious);

        // Findings list should have merged the two classes into a single finding
        int dropperFindingCount = 0;
        QString mergedClasses;
        for (const auto& f : report.findings) {
            if (f.ruleId == "combo_network_dropper") {
                dropperFindingCount++;
                mergedClasses = f.className;
            }
        }
        QCOMPARE(dropperFindingCount, 1);
        QVERIFY(mergedClasses.contains("DownloaderA"));
        QVERIFY(mergedClasses.contains("DownloaderB"));
    }

    void test_ModAuditor_ExpandedWhitelistAndDesktop()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        // 1. Moulberry
        QString moulberryJar = buildTestJar(tempDir.path(), "com/moulberry/axiom/Axiom.class",
                                            {}, { "java/lang/ProcessBuilder", "java/net/URL" });
        QVERIFY(!moulberryJar.isEmpty());
        auto reportMoulberry = ModAuditor::auditJar(moulberryJar);
        QCOMPARE(reportMoulberry.riskScore, 0);
        QCOMPARE(reportMoulberry.verdict, AuditVerdict::Clean);

        // 2. IsXander
        QString isxanderJar = buildTestJar(tempDir.path(), "dev/isxander/yacl/Config.class",
                                           {}, { "java/lang/ProcessBuilder" });
        QVERIFY(!isxanderJar.isEmpty());
        auto reportIsxander = ModAuditor::auditJar(isxanderJar);
        QCOMPARE(reportIsxander.riskScore, 0);
        QCOMPARE(reportIsxander.verdict, AuditVerdict::Clean);

        // 3. Euphoria Patcher
        QString euphoriaJar = buildTestJar(tempDir.path(), "euphoriapatches/ShaderUpdater.class",
                                           {}, { "java/net/URLClassLoader", "java/net/URL" });
        QVERIFY(!euphoriaJar.isEmpty());
        auto reportEuphoria = ModAuditor::auditJar(euphoriaJar);
        QCOMPARE(reportEuphoria.riskScore, 0);
        QCOMPARE(reportEuphoria.verdict, AuditVerdict::Clean);

        // 4. e4steam
        QString e4steamJar = buildTestJar(tempDir.path(), "e4steam/P2PNetworking.class",
                                          {}, { "java/net/Socket", "java/lang/ProcessBuilder" });
        QVERIFY(!e4steamJar.isEmpty());
        auto reportE4steam = ModAuditor::auditJar(e4steamJar);
        QCOMPARE(reportE4steam.riskScore, 0);
        QCOMPARE(reportE4steam.verdict, AuditVerdict::Clean);

        // 5. MixinExtras bundled library
        QString mixinExtrasJar = buildTestJar(tempDir.path(), "com/llamalad7/mixinextras/injector/DynamicInjector.class",
                                              { "defineClass" }, { "java/net/URLClassLoader", "java/lang/ProcessBuilder" });
        QVERIFY(!mixinExtrasJar.isEmpty());
        auto reportMixinExtras = ModAuditor::auditJar(mixinExtrasJar);
        QCOMPARE(reportMixinExtras.riskScore, 0);
        QCOMPARE(reportMixinExtras.verdict, AuditVerdict::Clean);

        // 6. FabricMC bundled library
        QString fabricJar = buildTestJar(tempDir.path(), "net/fabricmc/loader/impl/launch/knot/KknotClassLoader.class",
                                         { "defineClass" }, { "java/net/URLClassLoader" });
        QVERIFY(!fabricJar.isEmpty());
        auto reportFabric = ModAuditor::auditJar(fabricJar);
        QCOMPARE(reportFabric.riskScore, 0);
        QCOMPARE(reportFabric.verdict, AuditVerdict::Clean);

        // 7. Desktop / Browser link opening (xdg-open / explorer.exe)
        // Must NOT trigger dropper even if URL is also present
        QString browserJar = buildTestJar(tempDir.path(), "com/mod/ui/WebLinkButton.class",
                                          { "xdg-open", "explorer.exe", "https://modrinth.com" },
                                          { "java/lang/ProcessBuilder", "java/net/URL" });
        QVERIFY(!browserJar.isEmpty());
        auto reportBrowser = ModAuditor::auditJar(browserJar);
        QCOMPARE(reportBrowser.riskScore, 0);
        QCOMPARE(reportBrowser.verdict, AuditVerdict::Clean);
    }

    void test_ModAuditor_DiscordInviteVsWebhook()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        // 1. Benign Discord invite link or discord.gg URL:
        // Must NOT trigger exfiltration warning (score 0, Clean)
        QString inviteJar = buildTestJar(tempDir.path(), "com/mod/gui/ModSettings.class",
                                         { "https://discord.com/invite/abcdef", "https://discord.gg/xyz123" });
        QVERIFY(!inviteJar.isEmpty());
        auto reportInvite = ModAuditor::auditJar(inviteJar);
        QCOMPARE(reportInvite.riskScore, 0);
        QCOMPARE(reportInvite.verdict, AuditVerdict::Clean);

        // 2. Real Discord webhook endpoint:
        // MUST trigger exfiltration (score 80, Suspicious / Dangerous)
        QString webhookJar = buildTestJar(tempDir.path(), "com/mod/stealer/Notifier.class",
                                          { "https://discord.com/api/webhooks/123456789/abcdefgh" });
        QVERIFY(!webhookJar.isEmpty());
        auto reportWebhook = ModAuditor::auditJar(webhookJar);
        QVERIFY(reportWebhook.riskScore >= 80);
        bool foundWebhook = false;
        for (const auto& f : reportWebhook.findings) {
            if (f.ruleId == "data_exfil") {
                foundWebhook = true;
                break;
            }
        }
        QVERIFY(foundWebhook);
    }

    void test_ModAuditor_TrustedMods()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QString testJar = buildTestJar(tempDir.path(), "com/mod/test/TestMod.class", { "hello" });
        QVERIFY(!testJar.isEmpty());

        // Initially not trusted
        ModAuditor::clearTrustedModsCache();
        QVERIFY(!ModAuditor::isModTrusted(testJar));

        // Add to trusted
        ModAuditor::setModTrusted(testJar, true);
        QVERIFY(ModAuditor::isModTrusted(testJar));

        // Disabled file name variant should also be considered trusted
        QVERIFY(ModAuditor::isModTrusted(testJar + ".disabled"));

        // Remove from trusted
        ModAuditor::setModTrusted(testJar, false);
        QVERIFY(!ModAuditor::isModTrusted(testJar));
    }

    void test_CrashAnalyzer_ModResolverAndConflict()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QString modsPath = tempDir.filePath("mods");
        QDir().mkpath(modsPath);

        QString sodiumJar = buildTestJar(modsPath, "me/jellysquid/mods/sodium/client/Sodium.class", { "sodium.mixins.core.json" });
        QVERIFY(!sodiumJar.isEmpty());
        QFile::rename(sodiumJar, modsPath + "/sodium-fabric-0.5.8.jar");

        QString irisJar = buildTestJar(modsPath, "net/coderbot/iris/Iris.class", { "iris.mixins.json" });
        QVERIFY(!irisJar.isEmpty());
        QFile::rename(irisJar, modsPath + "/iris-fabric-1.6.4.jar");

        // Test Mod Resolver
        QString resolvedSodium = CrashAnalyzer::resolveModJar(tempDir.path(), "sodium.mixins.core.json");
        QCOMPARE(resolvedSodium, QStringLiteral("sodium-fabric-0.5.8.jar"));

        QString resolvedIris = CrashAnalyzer::resolveModJar(tempDir.path(), "iris.mixins.json");
        QCOMPARE(resolvedIris, QStringLiteral("iris-fabric-1.6.4.jar"));

        // Package resolution
        QString resolvedByPkg = CrashAnalyzer::resolveModJar(tempDir.path(), "me.jellysquid.mods.sodium.client.Sodium");
        QCOMPARE(resolvedByPkg, QStringLiteral("sodium-fabric-0.5.8.jar"));

        // Test conflict log with both mods
        QString log =
            "org.spongepowered.asm.mixin.transformer.throwables.MixinTransformerError: An unexpected critical error was encountered\n"
            "Caused by: org.spongepowered.asm.mixin.throwables.MixinApplyError: Mixin [sodium.mixins.core.json:client.MixinMinecraftClient from mod sodium] from phase [DEFAULT] in config [sodium.mixins.core.json] FAILED during APPLY\n"
            "Caused by: org.spongepowered.asm.mixin.injection.throwables.InvalidInjectionException: Critical injection failure: cancelled by [iris.mixins.json] from mod iris\n";

        auto issues = CrashAnalyzer::analyze(log, tempDir.path());
        QVERIFY(!issues.isEmpty());
        auto issue = issues.first();
        QCOMPARE(issue.id, QStringLiteral("mixin_failure"));
        // Both mods should be available as action buttons to disable, plus the option to disable both!
        QCOMPARE(issue.actions.size(), 3);
        QCOMPARE(issue.actions[0].type, CrashActionType::DisableCulpritMod);
        QCOMPARE(issue.actions[0].target, QStringLiteral("sodium-fabric-0.5.8.jar"));
        QCOMPARE(issue.actions[1].type, CrashActionType::DisableCulpritMod);
        QCOMPARE(issue.actions[1].target, QStringLiteral("iris-fabric-1.6.4.jar"));
        QCOMPARE(issue.actions[2].type, CrashActionType::DisableBothMods);
        QCOMPARE(issue.actions[2].target, QStringLiteral("sodium-fabric-0.5.8.jar|iris-fabric-1.6.4.jar"));
    }

    void test_CrashAnalyzer_CorruptedConfigNightConfig()
    {
        QString log =
            "[main/FATAL] [neoforge/]: Failed loading config file create-client.toml of type CLIENT for modid create\n"
            "com.electronwill.nightconfig.core.io.ParsingException: Not enough data available\n"
            "    at com.electronwill.nightconfig.core.io.ParsingException.notEnoughData(ParsingException.java:22)\n";

        auto issues = CrashAnalyzer::analyze(log);
        QVERIFY(!issues.isEmpty());
        auto issue = issues.first();
        QCOMPARE(issue.id, QStringLiteral("corrupted_config"));
        QVERIFY(!issue.actions.isEmpty());
        QCOMPARE(issue.actions.first().type, CrashActionType::ResetCorruptedConfig);
        QCOMPARE(issue.actions.first().target, QStringLiteral("create-client.toml"));
    }

    void test_CrashAnalyzer_TickingEntity()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QString modsPath = tempDir.filePath("mods");
        QDir().mkpath(modsPath);

        QString hydraJar = buildTestJar(modsPath, "twilightforest/entity/boss/HydraEntity.class", {});
        QVERIFY(!hydraJar.isEmpty());
        QFile::rename(hydraJar, modsPath + "/twilightforest-1.20.1-4.3.2145.jar");

        QString log =
            "Description: Ticking entity\n\n"
            "java.lang.NullPointerException: Cannot invoke method\n"
            "-- Entity being ticked --\n"
            "Details:\n"
            "\tEntity Type: twilightforest:hydra (twilightforest.entity.boss.HydraEntity)\n"
            "\tEntity ID: 1234\n"
            "\tEntity Name: Hydra\n"
            "\tExact location: -120.50, 64.00, 305.20\n";

        auto issues = CrashAnalyzer::analyze(log, tempDir.path());
        QVERIFY(!issues.isEmpty());
        auto issue = issues.first();
        QCOMPARE(issue.id, QStringLiteral("ticking_entity"));
        QVERIFY(issue.details.contains("twilightforest:hydra"));
        QVERIFY(issue.details.contains("-120.50, 64.00, 305.20"));

        // Should have 2 actions: disable mod + enable Forge auto-removal
        QCOMPARE(issue.actions.size(), 2);
        QCOMPARE(issue.actions[0].type, CrashActionType::DisableCulpritMod);
        QCOMPARE(issue.actions[0].target, QStringLiteral("twilightforest-1.20.1-4.3.2145.jar"));
        QCOMPARE(issue.actions[1].type, CrashActionType::EnableForgeRemoveErroringEntities);
    }

    void test_CrashAnalyzer_ShaderAndResourcePack()
    {
        // 1. Shader error
        QString shaderLog =
            "[Render thread/ERROR]: [Iris] Error creating shader program: Composite1\n"
            "net.coderbot.iris.pipeline.newshader.ShaderException: Program linked with compile errors\n";

        auto shaderIssues = CrashAnalyzer::analyze(shaderLog);
        QVERIFY(!shaderIssues.isEmpty());
        QCOMPARE(shaderIssues.first().id, QStringLiteral("shader_crash"));
        QCOMPARE(shaderIssues.first().actions.first().type, CrashActionType::DisableShaders);

        // 2. Texture Stitcher / Resource pack error
        QString rpLog =
            "[Render thread/ERROR]: net.minecraft.client.renderer.texture.StitcherException: Unable to fit: example:textures/block/huge.png, size: 2048x2048, atlas: 4096x4096\n";

        auto rpIssues = CrashAnalyzer::analyze(rpLog);
        QVERIFY(!rpIssues.isEmpty());
        QCOMPARE(rpIssues.first().id, QStringLiteral("resource_pack_crash"));
        QCOMPARE(rpIssues.first().actions.first().type, CrashActionType::ResetResourcePacks);
    }

    void test_CrashAnalyzer_OutOfMemoryHeap()
    {
        QString log = "java.lang.OutOfMemoryError: Java heap space\n";
        auto issues = CrashAnalyzer::analyze(log);
        QVERIFY(!issues.isEmpty());
        QCOMPARE(issues.first().id, QStringLiteral("out_of_memory"));
        QCOMPARE(issues.first().actions.first().type, CrashActionType::IncreaseAllocatedMemory);
    }

    void test_CrashAnalyzer_MixinConflictCombatantPunchyNoAlphabeticalBias()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QString modsPath = tempDir.filePath("minecraft/mods");
        QDir().mkpath(modsPath);

        // Create 3 mods: appleskin (alphabetically first), combatant, punchy
        QString appleJar = buildTestJar(modsPath, "squeek/appleskin/AppleSkin.class", {});
        QFile::rename(appleJar, modsPath + "/appleskin-fabric-mc26.2-3.0.10.jar");

        QString combatantJar = buildTestJar(modsPath, "com/combatant/mixins.json", {});
        QFile::rename(combatantJar, modsPath + "/combatant-fabric-1.20.1-1.0.jar");

        QString punchyJar = buildTestJar(modsPath, "com/punchy/mixins.json", {});
        QFile::rename(punchyJar, modsPath + "/punchy-fabric-0.5.jar");

        // Log has appleskin at top (loading mods) but the crash is between combatant and punchy
        QString log =
            "[FabricLoader/Metadata]: Loading 150 mods: from mod [appleskin] (appleskin-fabric-mc26.2-3.0.10.jar), ...\n"
            "[Render thread/INFO]: [Iris] Found shaderpack: ComplementaryReimagined_r5.1.1.zip\n"
            "[main/ERROR]: Minecraft has crashed!\n"
            "org.spongepowered.asm.mixin.transformer.throwables.MixinTransformerError: An unexpected critical error was encountered\n"
            "Caused by: org.spongepowered.asm.mixin.injection.throwables.InjectionError: "
            "Critical injection failure: Redirector combatant$redirectMethod()Z in combatant.mixins.json failed injection check : cancelled by punchy.mixins.json from mod punchy\n";

        auto issues = CrashAnalyzer::analyze(log, tempDir.path());
        QVERIFY(!issues.isEmpty());
        auto issue = issues.first();
        QCOMPARE(issue.id, QStringLiteral("mixin_failure"));

        // Crucial check: appleskin must NEVER be picked!
        QVERIFY(!issue.title.contains("appleskin", Qt::CaseInsensitive));
        QVERIFY(!issue.details.contains("appleskin", Qt::CaseInsensitive));
        for (const auto& f : issue.affectedFiles) {
            QVERIFY(!f.contains("appleskin", Qt::CaseInsensitive));
        }

        // Both combatant and punchy should be detected and offered as action buttons, plus disable both!
        QCOMPARE(issue.actions.size(), 3);
        QString t0 = issue.actions[0].target;
        QString t1 = issue.actions[1].target;
        QString t2 = issue.actions[2].target;
        QVERIFY(t0.contains("combatant") || t0.contains("punchy"));
        QVERIFY(t1.contains("combatant") || t1.contains("punchy"));
        QVERIFY(t0 != t1);
        QCOMPARE(issue.actions[2].type, CrashActionType::DisableBothMods);
        QVERIFY(t2.contains("combatant") && t2.contains("punchy"));

        // Also verify shaders false positive is NOT triggered by the [INFO] line in this log!
        for (const auto& iss : issues) {
            QVERIFY(iss.id != QStringLiteral("shader_crash"));
        }
    }

    void test_CrashAnalyzer_ShaderBenignInfoNoFalsePositive()
    {
        QString log =
            "[Render thread/INFO]: [Iris] Found shaderpack: ComplementaryReimagined_r5.1.1.zip\n"
            "[Render thread/INFO]: [Iris] Shaders initialized with optionsshaders.txt\n"
            "[main/INFO]: Loaded ShaderPack successfully\n";

        auto issues = CrashAnalyzer::analyze(log);
        for (const auto& issue : issues) {
            QVERIFY(issue.id != QStringLiteral("shader_crash"));
        }
    }

    void test_ModAuditor_DiskPersistenceUntilUpdate()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QString modJar = buildTestJar(tempDir.path(), "com/example/MyMod.class", { "clean_string" });
        QVERIFY(!modJar.isEmpty());

        // 1. Audit and ensure report is produced and cached
        auto report1 = ModAuditor::auditJar(modJar, true);
        QCOMPARE(report1.riskScore, 0);
        QVERIFY(ModAuditor::hasCachedReport(modJar));

        // 2. Fetch from cache
        auto cached = ModAuditor::getCachedReport(modJar);
        QCOMPARE(cached.riskScore, 0);
        QCOMPARE(cached.verdict, AuditVerdict::Clean);

        // 3. Ensure persistent disk cache file exists
        QFile cacheFile(ModAuditor::getAuditCacheFilePath());
        QVERIFY(cacheFile.exists());

        // 4. Update the mod file (simulate user updating the mod)
        QTest::qWait(1050); // wait >1s so lastModified timestamp changes
        QFile f(modJar);
        QVERIFY(f.open(QIODevice::Append));
        f.write("MOD_UPDATE_APPEND_BYTES");
        f.close();

        // 5. Cache MUST detect the update and invalidate!
        QVERIFY(!ModAuditor::hasCachedReport(modJar));
    }

    void test_AutoScanDownloadedMods_SettingOverride()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        auto globalSettings = std::make_shared<INISettingsObject>(tempDir.filePath("global.ini"));
        globalSettings->registerSetting("AutoScanDownloadedMods", true);
        QCOMPARE(globalSettings->get("AutoScanDownloadedMods").toBool(), true);

        auto instanceSettings = std::make_shared<INISettingsObject>(tempDir.filePath("instance.ini"));
        auto overrideSetting = instanceSettings->registerSetting("OverrideModSecurity", false);
        instanceSettings->registerOverride(globalSettings->getSetting("AutoScanDownloadedMods"), overrideSetting);

        // Initially inherits global true
        QCOMPARE(instanceSettings->get("AutoScanDownloadedMods").toBool(), true);

        // Change global to false -> instance inherits false
        globalSettings->set("AutoScanDownloadedMods", false);
        QCOMPARE(instanceSettings->get("AutoScanDownloadedMods").toBool(), false);

        // Enable override on instance and set to true
        overrideSetting->set(true);
        instanceSettings->set("AutoScanDownloadedMods", true);
        QCOMPARE(instanceSettings->get("AutoScanDownloadedMods").toBool(), true);

        // Global is still false
        QCOMPARE(globalSettings->get("AutoScanDownloadedMods").toBool(), false);

        // Disable override -> reverts back to global false
        overrideSetting->set(false);
        QCOMPARE(instanceSettings->get("AutoScanDownloadedMods").toBool(), false);
    }

    void test_CrashAnalyzer_MixinConflictScreenEffectRendererTwoMods()
    {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QString modsPath = tempDir.filePath("minecraft/mods");
        QDir().mkpath(modsPath);

        // Mod 1: combatant
        QString combatantJar = buildTestJar(modsPath, "net/minecraft/client/renderer/ScreenEffectRendererMixin.class", {});
        QFile::rename(combatantJar, modsPath + "/combatant-0.2.0.jar");

        // Mod 2: punchy (which also modifies ScreenEffectRenderer)
        QString punchyJar = buildTestJar(modsPath, "net/minecraft/client/renderer/ScreenEffectRendererMixin.class", {});
        QFile::rename(punchyJar, modsPath + "/punchy-1.0.jar");

        // The exact log error line from user's screenshot:
        QString log =
            "Caused by: org.spongepowered.asm.mixin.injection.throwables.InjectionError: "
            "Critical injection failure: Redirector combatant$maybeRenderFireOverlay(Lcom/mojang/blaze3d/vertex/PoseStack;Lnet/minecraft/client/renderer/SubmitNodeCollector;Lnet/minecraft/client/renderer/texture/TextureAtlasSprite;)V "
            "in combatant.mixins.json:ScreenEffectRendererMixin from mod combatant failed injection check, (0/1) succeeded. Scanned 0 target(s). No refMap loaded.\n";

        auto issues = CrashAnalyzer::analyze(log, tempDir.path());
        QVERIFY(!issues.isEmpty());
        auto issue = issues.first();
        QCOMPARE(issue.id, QStringLiteral("mixin_failure"));

        // Both combatant and punchy should be detected and identified as conflicting on ScreenEffectRenderer!
        QVERIFY(issue.title.contains("combatant") && issue.title.contains("punchy"));
        QCOMPARE(issue.actions.size(), 3);

        // Action 1: Disable only combatant
        // Action 2: Disable only punchy
        // Action 3: Disable both
        QCOMPARE(issue.actions[0].type, CrashActionType::DisableCulpritMod);
        QCOMPARE(issue.actions[1].type, CrashActionType::DisableCulpritMod);
        QCOMPARE(issue.actions[2].type, CrashActionType::DisableBothMods);
        QVERIFY(issue.actions[0].target != issue.actions[1].target);
    }
};

QTEST_GUILESS_MAIN(DiagnosticsTest)

#include "Diagnostics_test.moc"
