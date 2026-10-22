#include <QTest>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QBuffer>
#include <QDir>

#include "tools/configs/ConfigDocument.h"
#include "tools/configs/ModLangResolver.h"

using namespace ConfigEditor;

class ConfigDocumentTest : public QObject {
    Q_OBJECT

private slots:
    void test_TomlParsing()
    {
        QString toml =
            "# Client configuration file\n"
            "[client]\n"
            "# Whether to enable custom animations\n"
            "enable_animations = true\n"
            "\n"
            "# UI scale multiplier\n"
            "# Range: 0.5 ~ 4.0\n"
            "ui_scale = 1.5\n"
            "\n"
            "# Graphics quality preset\n"
            "# Allowed Values: FAST, FANCY, FABULOUS\n"
            "graphics_preset = \"FANCY\"\n"
            "\n"
            "# Max render distance in chunks\n"
            "# Range: 2 ~ 64\n"
            "max_chunks = 32\n";

        ConfigDocument doc;
        QString err;
        QVERIFY(doc.loadFromText(toml, FileFormat::Toml, "client.toml", &err));

        const auto& sections = doc.sections();
        QCOMPARE(sections.size(), 1);
        QCOMPARE(sections[0].name, QStringLiteral("client"));
        QCOMPARE(sections[0].fields.size(), 4);

        // Field 0: Boolean
        const auto& f0 = sections[0].fields[0];
        QCOMPARE(f0.key, QStringLiteral("enable_animations"));
        QCOMPARE(f0.type, FieldType::Boolean);
        QCOMPARE(f0.currentValue, QStringLiteral("true"));
        QVERIFY(f0.description.contains("Whether to enable custom animations"));

        // Field 1: Double with Range
        const auto& f1 = sections[0].fields[1];
        QCOMPARE(f1.key, QStringLiteral("ui_scale"));
        QCOMPARE(f1.type, FieldType::Double);
        QCOMPARE(f1.currentValue, QStringLiteral("1.5"));
        QVERIFY(f1.hasRange);
        QCOMPARE(f1.rangeMin, 0.5);
        QCOMPARE(f1.rangeMax, 4.0);

        // Field 2: Enum with Allowed Values
        const auto& f2 = sections[0].fields[2];
        QCOMPARE(f2.key, QStringLiteral("graphics_preset"));
        QCOMPARE(f2.type, FieldType::Enum);
        QCOMPARE(f2.currentValue, QStringLiteral("FANCY"));
        QCOMPARE(f2.allowedValues.size(), 3);
        QVERIFY(f2.allowedValues.contains("FAST"));
        QVERIFY(f2.allowedValues.contains("FABULOUS"));
        QVERIFY(f2.isQuoted);

        // Field 3: Integer with Range
        const auto& f3 = sections[0].fields[3];
        QCOMPARE(f3.key, QStringLiteral("max_chunks"));
        QCOMPARE(f3.type, FieldType::Integer);
        QCOMPARE(f3.currentValue, QStringLiteral("32"));
        QVERIFY(f3.hasRange);
        QCOMPARE(f3.rangeMin, 2.0);
        QCOMPARE(f3.rangeMax, 64.0);
    }

    void test_NonDestructiveTomlUpdate()
    {
        QString toml =
            "# Header comment that must be 100% preserved\n"
            "[general]\n"
            "   # Indented comment\n"
            "   fov = 70.0 # Inline comment here\n"
            "   greeting = \"Hello World\"\n"
            "   debug = false\n";

        ConfigDocument doc;
        QVERIFY(doc.loadFromText(toml, FileFormat::Toml, "settings.toml"));

        // Update fields
        QVERIFY(doc.updateFieldValue("general", "fov", "90.0"));
        QVERIFY(doc.updateFieldValue("general", "greeting", "Welcome"));
        QVERIFY(doc.updateFieldValue("general", "debug", "true"));

        QString updated = doc.generateText();

        // Verify comments and indentation are preserved
        QVERIFY(updated.contains("# Header comment that must be 100% preserved"));
        QVERIFY(updated.contains("   # Indented comment"));
        // Value updated while preserving indentation, '=' and inline comment
        QVERIFY(updated.contains("   fov = 90.0 # Inline comment here"));
        // Quotes preserved
        QVERIFY(updated.contains("   greeting = \"Welcome\""));
        QVERIFY(updated.contains("   debug = true"));
    }

    void test_CfgForgeParsing()
    {
        QString cfg =
            "# Forge configuration file\n"
            "general {\n"
            "    # Enable special effects\n"
            "    B:enableEffects=true\n"
            "\n"
            "    # Particle count\n"
            "    # Range: 0 ~ 500\n"
            "    I:particles=150\n"
            "\n"
            "    # Sound volume\n"
            "    D:volume=0.85\n"
            "\n"
            "    S:customName=\"Player1\"\n"
            "}\n";

        ConfigDocument doc;
        QVERIFY(doc.loadFromText(cfg, FileFormat::Cfg, "mod.cfg"));

        const auto& sections = doc.sections();
        QCOMPARE(sections.size(), 1);
        QCOMPARE(sections[0].name, QStringLiteral("general"));
        QCOMPARE(sections[0].fields.size(), 4);

        QCOMPARE(sections[0].fields[0].key, QStringLiteral("enableEffects"));
        QCOMPARE(sections[0].fields[0].type, FieldType::Boolean);
        QCOMPARE(sections[0].fields[0].currentValue, QStringLiteral("true"));

        QCOMPARE(sections[0].fields[1].key, QStringLiteral("particles"));
        QCOMPARE(sections[0].fields[1].type, FieldType::Integer);
        QCOMPARE(sections[0].fields[1].currentValue, QStringLiteral("150"));

        QCOMPARE(sections[0].fields[2].key, QStringLiteral("volume"));
        QCOMPARE(sections[0].fields[2].type, FieldType::Double);

        QCOMPARE(sections[0].fields[3].key, QStringLiteral("customName"));
        QCOMPARE(sections[0].fields[3].currentValue, QStringLiteral("Player1"));
        QVERIFY(sections[0].fields[3].isQuoted);

        // Update particle count and verify B:/I: prefix preserved
        doc.updateFieldValue("general", "particles", "300");
        QString updated = doc.generateText();
        QVERIFY(updated.contains("I:particles=300"));
        QVERIFY(updated.contains("B:enableEffects=true"));
    }

    void test_OptionsTxtParsing()
    {
        QString options =
            "version:3465\n"
            "fov:0.0\n"
            "gamma:1.0\n"
            "soundCategory_master:0.8\n"
            "soundCategory_music:0.5\n"
            "key_key.jump:key.keyboard.space\n";

        ConfigDocument doc;
        QVERIFY(doc.loadFromText(options, FileFormat::Txt, "options.txt"));

        // Check logical section grouping
        bool hasGame = false;
        bool hasAudio = false;
        bool hasControls = false;

        for (const auto& sec : doc.sections()) {
            if (sec.name == "Game Options") hasGame = true;
            if (sec.name == "Audio / Sound") hasAudio = true;
            if (sec.name == "Controls / Keybinds") hasControls = true;
        }

        QVERIFY(hasGame);
        QVERIFY(hasAudio);
        QVERIFY(hasControls);

        // Update fov
        doc.updateFieldValue("Game Options", "fov", "0.5");
        QString updated = doc.generateText();
        QVERIFY(updated.contains("fov:0.5"));
    }

    void test_JsonParsingWithComments()
    {
        QString json =
            "{\n"
            "  // This is a comment\n"
            "  \"general\": {\n"
            "    \"enableHud\": true,\n"
            "    \"hudScale\": 1.25,\n"
            "    /* Multi-line\n"
            "       comment */\n"
            "    \"mode\": \"COMPACT\"\n"
            "  },\n"
            "  \"maxItems\": 20,\n"
            "}\n";

        ConfigDocument doc;
        QVERIFY(doc.loadFromText(json, FileFormat::Json, "appleskin.json"));

        QVERIFY(doc.sections().size() >= 1);

        doc.updateFieldValue("general", "enableHud", "false");
        QString updated = doc.generateText();
        QVERIFY(updated.contains("\"enableHud\": false"));
    }

    void test_RussianLocaleDoubleFormatting()
    {
        // Force Russian locale where decimal separator is normally comma ','
        QLocale oldLocale = QLocale();
        QLocale::setDefault(QLocale(QLocale::Russian, QLocale::Russia));

        QString toml =
            "[graphics]\n"
            "scale = 0.75\n"
            "gamma = 1.0\n";

        ConfigDocument doc;
        QVERIFY(doc.loadFromText(toml, FileFormat::Toml, "graphics.toml"));

        // Format double strictly with dot and no trailing zeros
        double newVal = 1.25;
        QString formattedVal = QString::number(newVal, 'f', 6);
        while (formattedVal.contains('.') && formattedVal.endsWith('0')) {
            formattedVal.chop(1);
        }
        if (formattedVal.endsWith('.')) {
            formattedVal.chop(1);
        }

        QVERIFY(doc.updateFieldValue("graphics", "scale", formattedVal));

        QString updated = doc.generateText();
        QVERIFY(updated.contains("scale = 1.25"));
        QVERIFY(!updated.contains("1,25"));

        // Restore locale
        QLocale::setDefault(oldLocale);
    }

    void test_LegacyForgeCfgRangeAndDefaults()
    {
        QString cfg =
            "# Configuration file\n"
            "general {\n"
            "    # Multiplier for particle spawning\n"
            "    # [range: 0 ~ 500, default: 100]\n"
            "    I:maxParticles=100\n"
            "\n"
            "    # Custom entity scale factor\n"
            "    # [range: 0.1 ~ 5.0, default: 1.0]\n"
            "    D:scaleMultiplier=1.0\n"
            "\n"
            "    # Toggle ocelots sitting on chests\n"
            "    # [default: true]\n"
            "    B:ocelotSitChests=true\n"
            "}\n";

        ConfigDocument doc;
        QVERIFY(doc.loadFromText(cfg, FileFormat::Cfg, "ironchest.cfg"));

        const auto& sections = doc.sections();
        QCOMPARE(sections.size(), 1);
        QCOMPARE(sections[0].name, QStringLiteral("general"));
        QCOMPARE(sections[0].fields.size(), 3);

        // Field 0: Integer with old Forge [range: 0 ~ 500, default: 100]
        const auto& f0 = sections[0].fields[0];
        QCOMPARE(f0.rawKey, QStringLiteral("I:maxParticles"));
        QCOMPARE(f0.key, QStringLiteral("maxParticles")); // Prefix stripped for UI
        QCOMPARE(f0.type, FieldType::Integer);
        QVERIFY(f0.hasRange);
        QCOMPARE(f0.rangeMin, 0.0);
        QCOMPARE(f0.rangeMax, 500.0);
        QCOMPARE(f0.defaultValue, QStringLiteral("100"));
        QVERIFY(f0.description.contains("Multiplier for particle spawning"));
        QVERIFY(f0.description.contains("100")); // Contains default info

        // Field 1: Double with old Forge [range: 0.1 ~ 5.0, default: 1.0]
        const auto& f1 = sections[0].fields[1];
        QCOMPARE(f1.rawKey, QStringLiteral("D:scaleMultiplier"));
        QCOMPARE(f1.key, QStringLiteral("scaleMultiplier"));
        QCOMPARE(f1.type, FieldType::Double);
        QVERIFY(f1.hasRange);
        QCOMPARE(f1.rangeMin, 0.1);
        QCOMPARE(f1.rangeMax, 5.0);
        QCOMPARE(f1.defaultValue, QStringLiteral("1.0"));

        // Field 2: Boolean with B:
        const auto& f2 = sections[0].fields[2];
        QCOMPARE(f2.rawKey, QStringLiteral("B:ocelotSitChests"));
        QCOMPARE(f2.key, QStringLiteral("ocelotSitChests"));
        QCOMPARE(f2.type, FieldType::Boolean);
        QCOMPARE(f2.defaultValue, QStringLiteral("true"));

        // Saving preserves B:, I:, D: prefixes
        doc.updateFieldValue("general", "maxParticles", "250");
        QString updated = doc.generateText();
        QVERIFY(updated.contains("I:maxParticles=250"));
        QVERIFY(updated.contains("B:ocelotSitChests=true"));
        QVERIFY(updated.contains("D:scaleMultiplier=1.0"));
    }

    void test_LegacyForgeNestedSections()
    {
        QString cfg =
            "general {\n"
            "    B:mainEnable=true\n"
            "    client {\n"
            "        I:renderDistance=32\n"
            "    }\n"
            "    B:afterClient=false\n"
            "}\n";

        ConfigDocument doc;
        QVERIFY(doc.loadFromText(cfg, FileFormat::Cfg, "test.cfg"));

        const auto& sections = doc.sections();
        QCOMPARE(sections.size(), 2);
        QCOMPARE(sections[0].name, QStringLiteral("general"));
        QCOMPARE(sections[1].name, QStringLiteral("general.client"));

        QCOMPARE(sections[0].fields.size(), 2); // mainEnable and afterClient
        QCOMPARE(sections[1].fields.size(), 1); // renderDistance
    }

    void test_ModLangResolverCandidates()
    {
        QStringList c1 = ModLangResolver::candidateModIdsFromConfig("/path/to/config/appleskin.toml");
        QVERIFY(c1.contains("appleskin"));

        QStringList c2 = ModLangResolver::candidateModIdsFromConfig("/path/to/config/ironchest.cfg");
        QVERIFY(c2.contains("ironchest"));

        QStringList c3 = ModLangResolver::candidateModIdsFromConfig("/path/to/config/forestry/common.cfg");
        QVERIFY(c3.contains("forestry"));

        QStringList c4 = ModLangResolver::candidateModIdsFromConfig("/path/to/config/jei-client.toml");
        QVERIFY(c4.contains("jei"));
    }

    void test_ModLangResolverMatching()
    {
        QHash<QString, QString> langMap;
        // Legacy .lang format entries
        langMap["ironchest.config.ocelotSitChests"] = "Ocelots Sit on Chests";
        langMap["ironchest.config.ocelotSitChests.tooltip"] = "Toggles whether ocelots sit on iron chests.";

        // Modern .json format entries
        langMap["appleskin.config.show_food_values"] = "Show Food Values";
        langMap["appleskin.config.show_food_values.desc"] = "Shows food and saturation values on tooltips.";

        // Test title lookup
        QString t1 = ModLangResolver::findTitle(langMap, "general", "ocelotSitChests");
        QCOMPARE(t1, QStringLiteral("Ocelots Sit on Chests"));

        QString t2 = ModLangResolver::findTitle(langMap, "client", "show_food_values");
        QCOMPARE(t2, QStringLiteral("Show Food Values"));

        // Test description lookup
        QString d1 = ModLangResolver::findDescription(langMap, "general", "ocelotSitChests");
        QCOMPARE(d1, QStringLiteral("Toggles whether ocelots sit on iron chests."));

        QString d2 = ModLangResolver::findDescription(langMap, "client", "show_food_values");
        QCOMPARE(d2, QStringLiteral("Shows food and saturation values on tooltips."));
    }

    void test_BackupAndRestore()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QString filePath = dir.filePath("config.toml");
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("key = 10\n");
            f.close();
        }

        // Create backup
        QVERIFY(!ConfigDocument::hasBackup(filePath));
        QVERIFY(ConfigDocument::createBackup(filePath));
        QVERIFY(ConfigDocument::hasBackup(filePath));

        // Modify file
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("key = 999\n");
            f.close();
        }

        // Restore backup
        QVERIFY(ConfigDocument::restoreBackup(filePath));
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
            QString content = f.readAll();
            QCOMPARE(content.trimmed(), QStringLiteral("key = 10"));
        }
    }

    void test_SectionTitleFormatting()
    {
        QHash<QString, QString> langMap;
        langMap["modid.category.visuals"] = "Visual Settings";

        // From langMap
        QCOMPARE(ModLangResolver::resolveSectionTitle("visuals", langMap), QStringLiteral("Visual Settings"));

        // Fallback: CamelCase splitting
        QCOMPARE(ModLangResolver::resolveSectionTitle("negativeStatusEffectGainSoundEffect", {}),
                 QStringLiteral("Negative Status Effect Gain Sound Effect"));

        // Fallback: snake_case splitting
        QCOMPARE(ModLangResolver::resolveSectionTitle("client_settings", {}),
                 QStringLiteral("Client Settings"));

        // Fallback: dotted sections
        QCOMPARE(ModLangResolver::formatCamelOrSnakeCase("general.graphics_quality"),
                 QStringLiteral("General • Graphics Quality"));
    }

    void test_CommonKeyTranslations()
    {
        // Should find translations for common universal keys regardless of case
        QString t1 = ModLangResolver::lookupCommonKeyTranslation("shouldPlay");
        QVERIFY(!t1.isEmpty());

        QString t2 = ModLangResolver::lookupCommonKeyTranslation("soundEvent");
        QVERIFY(!t2.isEmpty());

        QString t3 = ModLangResolver::lookupCommonKeyTranslation("pitch");
        QVERIFY(!t3.isEmpty());

        QString t4 = ModLangResolver::lookupCommonKeyTranslation("volume");
        QVERIFY(!t4.isEmpty());

        QString t5 = ModLangResolver::lookupCommonKeyTranslation("nonExistentRandomKeyXYZ");
        QVERIFY(t5.isEmpty());
    }

    void test_KeybindFormatting()
    {
        // Test keybind display name formatting
        QString s1 = ModLangResolver::formatKeyDisplayName("key.keyboard.space");
        QVERIFY(s1.contains("Space", Qt::CaseInsensitive));

        QString s2 = ModLangResolver::formatKeyDisplayName("key.mouse.left");
        QVERIFY(s2.contains("Left", Qt::CaseInsensitive) || s2.contains("ЛКМ", Qt::CaseInsensitive));

        QString s3 = ModLangResolver::formatKeyDisplayName("key.keyboard.left.shift");
        QCOMPARE(s3, QStringLiteral("Left Shift"));

        QString s4 = ModLangResolver::formatKeyDisplayName("key.keyboard.f3");
        QCOMPARE(s4, QStringLiteral("F3"));

        QString s5 = ModLangResolver::formatKeyDisplayName("key.keyboard.r");
        QCOMPARE(s5, QStringLiteral("R"));

        QString s6 = ModLangResolver::formatKeyDisplayName("");
        QVERIFY(s6.contains("None", Qt::CaseInsensitive) || s6.contains("Не назначена", Qt::CaseInsensitive));
    }
};

QTEST_GUILESS_MAIN(ConfigDocumentTest)
#include "ConfigDocument_test.moc"

