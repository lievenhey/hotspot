/*
    SPDX-FileCopyrightText: Lieven Hey <lieven.hey@kdab.com>
    SPDX-FileCopyrightText: 2023 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "highlightedtext.h"

#include <QGuiApplication>
#include <QPalette>
#include <QTextLayout>

#include <memory>

#include <KColorScheme>

#if KFSyntaxHighlighting_FOUND
#include <KSyntaxHighlighting/AbstractHighlighter>
#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>
#endif

#include "formattingutils.h"

#if KFSyntaxHighlighting_FOUND
// highlighter using KSyntaxHighlighting
class HighlightingImplementation : public KSyntaxHighlighting::AbstractHighlighter
{
public:
    HighlightingImplementation(KSyntaxHighlighting::Repository* repository)
        : m_repository(repository)
    {
    }
    ~HighlightingImplementation() override = default;

    virtual QVector<QTextLayout::FormatRange> format(const QString& text)
    {
        m_formats.clear();

        highlightLine(text, {});

        return m_formats;
    }

    virtual void themeChanged()
    {
        if (!m_repository) {
            return;
        }

        KSyntaxHighlighting::Repository::DefaultTheme theme;
        const auto palette = QGuiApplication::palette();
        if (palette.base().color().lightness() < 128) {
            theme = KSyntaxHighlighting::Repository::DarkTheme;
        } else {
            theme = KSyntaxHighlighting::Repository::LightTheme;
        }
        setTheme(m_repository->defaultTheme(theme));
    }

    virtual void setHighlightingDefinition(const KSyntaxHighlighting::Definition& definition)
    {
        setDefinition(definition);
    }

    virtual QString definitionName() const
    {
        return definition().name();
    }

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format& format) override
    {
        QTextCharFormat textCharFormat;
        textCharFormat.setForeground(format.textColor(theme()));
        textCharFormat.setFontWeight(format.isBold(theme()) ? QFont::Bold : QFont::Normal);
        m_formats.push_back({offset, length, textCharFormat});
    }

private:
    KSyntaxHighlighting::Repository* m_repository;
    QVector<QTextLayout::FormatRange> m_formats;
};
#else
// stub incase KSyntaxHighlighting is not available
class HighlightingImplementation
{
public:
    HighlightingImplementation(KSyntaxHighlighting::Repository* /*repository*/) { }
    virtual ~HighlightingImplementation() = default;

    virtual QVector<QTextLayout::FormatRange> format(const QString& /*text*/)
    {
        return {};
    }

    virtual void themeChanged() { }

    virtual void setHighlightingDefinition(const KSyntaxHighlighting::Definition& /*definition*/) { }
    virtual QString definitionName() const
    {
        return {};
    }
};
#endif

class AnsiHighlightingImplementation : public HighlightingImplementation
{
public:
    AnsiHighlightingImplementation()
        : HighlightingImplementation(nullptr)
    {
    }
    ~AnsiHighlightingImplementation() override = default;

    QVector<QTextLayout::FormatRange> format(const QString& text) final
    {
        QVector<QTextLayout::FormatRange> formats;

        int offset = 0;
        QTextLayout::FormatRange format;

        constexpr int setColorSequenceLength = 5;
        constexpr int resetColorSequenceLength = 4;
        constexpr int colorCodeLength = 2;

        auto lastToken = text.begin();
        for (auto escapeIt = std::find(text.cbegin(), text.cend(), Util::escapeChar); escapeIt != text.cend();
             escapeIt = std::find(escapeIt, text.cend(), Util::escapeChar)) {

            Q_ASSERT(*(escapeIt + 1) == QLatin1Char('['));

            offset += std::distance(lastToken, escapeIt);

            // escapeIt + 2 points to the first color code character
            auto color = QStringView {escapeIt + 2, colorCodeLength};
            bool ok = false;
            const uint8_t colorCode = color.toUInt(&ok);
            if (ok) {
                // only support the 8 default colors
                Q_ASSERT(colorCode >= 30 && colorCode <= 37);

                format.start = offset;
                const auto colorRole = static_cast<KColorScheme::ForegroundRole>(colorCode - 30);
                format.format.setForeground(m_colorScheme.foreground(colorRole));

                std::advance(escapeIt, setColorSequenceLength);
            } else {
                // make sure we have a reset sequence
                Q_ASSERT(color == QStringLiteral("0m"));
                format.length = offset - format.start;
                if (format.length) {
                    formats.push_back(format);
                }

                std::advance(escapeIt, resetColorSequenceLength);
            }

            lastToken = escapeIt;
        }

        return formats;
    }

    void themeChanged() override
    {
        m_colorScheme = KColorScheme(QPalette::Normal, KColorScheme::Complementary);
    }

    void setHighlightingDefinition(const KSyntaxHighlighting::Definition& /*definition*/) override { }
    QString definitionName() const override
    {
        return {};
    }

private:
    KColorScheme m_colorScheme;
};

// QTextLayout is slow, this class acts as a cache that only creates and fills the QTextLayout on demand
class HighlightedLine
{
public:
    HighlightedLine(HighlightingImplementation* highlighter, QString text)
        : m_highlighter(highlighter)
        , m_text(std::move(text))
        , m_layout(nullptr)
    {
    }

    ~HighlightedLine() = default;

    HighlightedLine(HighlightedLine&&) = default;

    QTextLayout* layout()
    {
        if (!m_layout) {
            doLayout();
        }
        return m_layout.get();
    }

    void updateHighlighting()
    {
        m_layout = nullptr;
    }

private:
    void doLayout()
    {
        if (!m_layout) {
            m_layout = std::make_unique<QTextLayout>();
            m_layout->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            const auto& ansiFreeLine = Util::removeAnsi(m_text);
            m_layout->setText(ansiFreeLine);
        }

        m_layout->setFormats(m_highlighter->format(m_text));

        m_layout->beginLayout();

        // there is at most one line, so we don't need to check this multiple times
        QTextLine line = m_layout->createLine();
        if (line.isValid()) {
            line.setPosition(QPointF(0, 0));
        }
        m_layout->endLayout();
    }

    HighlightingImplementation* m_highlighter;
    QString m_text;
    std::unique_ptr<QTextLayout> m_layout;
};

HighlightedText::HighlightedText(KSyntaxHighlighting::Repository* repository, QObject* parent)
    : QObject(parent)
    , m_repository(repository)
{
}

HighlightedText::~HighlightedText() = default;

void HighlightedText::setText(const QStringList& text)
{
    const bool usesAnsi =
        std::any_of(text.cbegin(), text.cend(), [](const QString& line) { return line.contains(Util::escapeChar); });

    if (!m_highlighter || m_isUsingAnsi != usesAnsi) {
        if (usesAnsi) {
            m_highlighter = std::make_unique<AnsiHighlightingImplementation>();
        } else {
            m_highlighter = std::make_unique<HighlightingImplementation>(m_repository);
        }
        m_highlighter->themeChanged();
        m_isUsingAnsi = usesAnsi;
        emit usesAnsiChanged(usesAnsi);
    }

    m_highlightedLines.reserve(text.size());
    std::transform(text.cbegin(), text.cend(), std::back_inserter(m_highlightedLines), [this](const QString& text) {
        return HighlightedLine {m_highlighter.get(), text};
    });

    connect(this, &HighlightedText::definitionChanged, this, &HighlightedText::updateHighlighting);

    m_cleanedLines.reserve(text.size());
    std::transform(text.cbegin(), text.cend(), std::back_inserter(m_cleanedLines), Util::removeAnsi);
}

void HighlightedText::setDefinition(const KSyntaxHighlighting::Definition& definition)
{
    Q_ASSERT(m_highlighter);
    m_highlighter->setHighlightingDefinition(definition);
#if KFSyntaxHighlighting_FOUND
    emit definitionChanged(definition.name());
#endif
}

QString HighlightedText::textAt(int index) const
{
    Q_ASSERT(m_highlighter);
    return m_cleanedLines.at(index);
}

QTextLine HighlightedText::lineAt(int index) const
{
    auto& line = m_highlightedLines[index];
    return line.layout()->lineAt(0);
}

QString HighlightedText::definition() const
{
    if (!m_highlighter)
        return {};
    return m_highlighter->definitionName();
}

QTextLayout* HighlightedText::layoutForLine(int index)
{
    return m_highlightedLines[index].layout();
}

void HighlightedText::updateHighlighting()
{
    m_highlighter->themeChanged();
    std::for_each(m_highlightedLines.begin(), m_highlightedLines.end(),
                  [](HighlightedLine& line) { line.updateHighlighting(); });
}
