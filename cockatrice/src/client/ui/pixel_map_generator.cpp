#include "pixel_map_generator.h"

#include "pb/serverinfo_user.pb.h"

#include <QApplication>
#include <QDomDocument>
#include <QFile>
#include <QPainter>
#include <QPalette>
#include <QSvgRenderer>

#define DEFAULT_COLOR_UNREGISTERED "#32c8ec";
#define DEFAULT_COLOR_REGISTERED "#5ed900";
#define DEFAULT_COLOR_DONATOR "#8c5fd3";
#define DEFAULT_COLOR_MODERATOR_LEFT "#ffffff";
#define DEFAULT_COLOR_MODERATOR_RIGHT "#000000";
#define DEFAULT_COLOR_ADMIN "#ff2701";

/**
 * Loads in an svg from file and scales it without affecting image quality.
 *
 * @param svgPath The path to the svg file. Automatically appends ".svg" to the end if not present
 * @param size The desired size of the pixmap
 *
 * @return The svg loaded into a Pixmap with the given size, or an empty Pixmap if the loading failed.
 */
static QPixmap loadSvg(const QString &svgPath, const QSize &size)
{
    QString path = svgPath;
    if (!path.endsWith(".svg")) {
        path += ".svg";
    }

    QSvgRenderer svgRenderer(path);

    if (!svgRenderer.isValid()) {
        qCWarning(PixelMapGeneratorLog) << "Failed to load" << path;
        return {};
    }

    // Makes sure the pixmap is at least as large as the svg, so that we don't lose any detail.
    // QIcon.pixmap(size) will automatically scale down the image, but it won't scale it up.
    QPixmap pix(svgRenderer.defaultSize().expandedTo(size));
    pix.fill(Qt::transparent);

    QPainter pixPainter(&pix);
    svgRenderer.render(&pixPainter);

    // Converting the pixmap to a QIcon and back is the easiest way to scale down a svg without affecting image quality
    return QIcon(pix).pixmap(size);
}

QMap<QString, QPixmap> PhasePixmapGenerator::pmCache;

QPixmap PhasePixmapGenerator::generatePixmap(int height, QString name)
{
    QString key = name + QString::number(height);
    if (pmCache.contains(key))
        return pmCache.value(key);

    QPixmap pixmap = loadSvg("theme:phases/" + name, QSize(height, height));

    pmCache.insert(key, pixmap);
    return pixmap;
}

QMap<QString, QPixmap> CounterPixmapGenerator::pmCache;

QPixmap CounterPixmapGenerator::generatePixmap(int height, QString name, bool highlight)
{
    // The colorless counter is named "x" by the server but the file is named "general.svg"
    if (name == "x") {
        name = "general";
    }

    if (highlight)
        name.append("_highlight");
    QString key = name + QString::number(height);
    if (pmCache.contains(key))
        return pmCache.value(key);

    QPixmap pixmap = loadSvg("theme:counters/" + name, QSize(height, height));

    // fall back to colorless counter if the name can't be found
    if (pixmap.isNull()) {
        name = "general";
        if (highlight)
            name.append("_highlight");
        pixmap = loadSvg("theme:counters/" + name, QSize(height, height));
    }

    pmCache.insert(key, pixmap);
    return pixmap;
}

QPixmap PingPixmapGenerator::generatePixmap(int size, int value, int max)
{
    int key = size * 1000000 + max * 1000 + value;
    if (pmCache.contains(key))
        return pmCache.value(key);

    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    QColor color;
    if ((max == -1) || (value == -1))
        color = Qt::black;
    else
        color.setHsv(120 * (1.0 - ((double)value / max)), 255, 255);

    QRadialGradient g(QPointF((double)pixmap.width() / 2, (double)pixmap.height() / 2),
                      qMin(pixmap.width(), pixmap.height()) / 2.0);
    g.setColorAt(0, color);
    g.setColorAt(1, Qt::transparent);
    painter.fillRect(0, 0, pixmap.width(), pixmap.height(), QBrush(g));

    pmCache.insert(key, pixmap);

    return pixmap;
}

QMap<int, QPixmap> PingPixmapGenerator::pmCache;

QPixmap CountryPixmapGenerator::generatePixmap(int height, const QString &countryCode)
{
    if (countryCode.size() != 2)
        return QPixmap();
    QString key = countryCode + QString::number(height);
    if (pmCache.contains(key))
        return pmCache.value(key);

    int width = height * 2;
    QPixmap pixmap = loadSvg("theme:countries/" + countryCode.toLower(), QSize(width, height));

    QPainter painter(&pixmap);
    painter.setPen(Qt::black);

    // width/height offset was determined through trial-and-error
#ifdef Q_OS_WIN
    painter.drawRect(0, 0, pixmap.width() - 1, pixmap.height() - 1);
#else
    painter.drawRect(0, 0, pixmap.width() / 2, pixmap.height() / 2);
#endif

    pmCache.insert(key, pixmap);
    return pixmap;
}

QMap<QString, QPixmap> CountryPixmapGenerator::pmCache;

/**
 * Updates tags in the svg
 *
 * @param elem The svg
 * @param tagName tag with attribute to update
 * @param attrName attribute to be updated
 * @param idName id that the tag has to match
 * @param attrValue the value to update the attribute to
 */
void setAttrRecur(QDomElement &elem,
                  const QString &tagName,
                  const QString &attrName,
                  const QString &idName,
                  const QString &attrValue)
{
    if (elem.tagName().compare(tagName) == 0) {
        if (elem.attribute("id").compare(idName) == 0) {
            elem.setAttribute(attrName, attrValue);
        }
    }

    for (int i = 0; i < elem.childNodes().count(); i++) {
        if (!elem.childNodes().at(i).isElement()) {
            continue;
        }
        auto docElem = elem.childNodes().at(i).toElement();
        setAttrRecur(docElem, tagName, attrName, idName, attrValue);
    }
}

/**
 * Loads the usericon svg and fills in its colors.
 * The image is kept as a QIcon to preserve the image quality.
 *
 * Call icon.pixmap(w, h) in order to convert this icon into a pixmap with the given dimensions.
 * Avoid scaling the pixmap in other ways, as that destroys image quality.
 *
 * @param minSize If the dimensions of the source svg is smaller than this, then it will be scaled up to this size
 */
static QIcon loadAndColorSvg(const QString &iconPath,
                             const QString &colorLeft,
                             const std::optional<QString> &colorRight,
                             const int minSize)
{
    QFile file(iconPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(PixelMapGeneratorLog) << "Unable to open" << iconPath;
        return {};
    }

    const auto &baData = file.readAll();
    QDomDocument doc;
    doc.setContent(baData);

    auto docElem = doc.documentElement();

    setAttrRecur(docElem, "path", "fill", "left", colorLeft);
    if (colorRight.has_value()) {
        setAttrRecur(docElem, "path", "fill", "right", colorRight.value());
    }

    QSvgRenderer svgRenderer(doc.toByteArray());

    QPixmap pix(svgRenderer.defaultSize().expandedTo(QSize(minSize, minSize)));
    pix.fill(Qt::transparent);

    QPainter pixPainter(&pix);
    svgRenderer.render(&pixPainter);

    return QIcon(pix);
}

QPixmap UserLevelPixmapGenerator::generatePixmap(int height,
                                                 UserLevelFlags userLevel,
                                                 ServerInfo_User::PawnColorsOverride pawnColorsOverride,
                                                 bool isBuddy,
                                                 const QString &privLevel)
{
    return generateIcon(height, userLevel, pawnColorsOverride, isBuddy, privLevel).pixmap(height, height);
}

QIcon UserLevelPixmapGenerator::generateIcon(int minHeight,
                                             UserLevelFlags userLevel,
                                             ServerInfo_User::PawnColorsOverride pawnColorsOverride,
                                             bool isBuddy,
                                             const QString &privLevel)
{
    std::optional<QString> colorLeft = std::nullopt;
    if (pawnColorsOverride.has_left_side()) {
        colorLeft = QString::fromStdString(pawnColorsOverride.left_side());
    }

    std::optional<QString> colorRight = std::nullopt;
    if (pawnColorsOverride.has_right_side()) {
        colorRight = QString::fromStdString(pawnColorsOverride.right_side());
    }

    QString key = QString::number(minHeight * 10000) + ":" + static_cast<short>(userLevel) + ":" +
                  static_cast<short>(isBuddy) + ":" + privLevel.toLower() + ":" + colorLeft.value_or("") + ":" +
                  colorRight.value_or("");

    if (iconCache.contains(key)) {
        return iconCache.value(key);
    }

    QIcon icon = colorLeft.has_value()
                     ? generateIconWithColorOverride(minHeight, isBuddy, privLevel, colorLeft, colorRight)
                     : generateIconDefault(minHeight, userLevel, isBuddy, privLevel);

    iconCache.insert(key, icon);
    return icon;
}

QIcon UserLevelPixmapGenerator::generateIconDefault(int height,
                                                    UserLevelFlags userLevel,
                                                    bool isBuddy,
                                                    const QString &privLevel)
{
    QString iconType;
    if (isBuddy) {
        iconType = "star";
    } else if (privLevel.toLower() == "vip") {
        iconType = "pawn_vip";
    } else {
        iconType = "pawn";
    }

    QString arity = "single";
    QString colorLeft;
    std::optional<QString> colorRight = std::nullopt;

    if (userLevel.testFlag(ServerInfo_User::IsAdmin)) {
        colorLeft = DEFAULT_COLOR_ADMIN;
    } else if (userLevel.testFlag(ServerInfo_User::IsModerator)) {
        colorLeft = DEFAULT_COLOR_MODERATOR_LEFT;
        colorRight = DEFAULT_COLOR_MODERATOR_RIGHT;
        arity = "double";
    } else if (privLevel.toLower() == "donator") {
        colorLeft = DEFAULT_COLOR_DONATOR;
    } else if (userLevel.testFlag(ServerInfo_User::IsRegistered)) {
        colorLeft = DEFAULT_COLOR_REGISTERED;
    } else {
        colorLeft = DEFAULT_COLOR_UNREGISTERED;
    }

    QString iconPath = QString("theme:usericons/%1_%2.svg").arg(iconType, arity);

    return loadAndColorSvg(iconPath, colorLeft, colorRight, height);
}

QIcon UserLevelPixmapGenerator::generateIconWithColorOverride(int height,
                                                              bool isBuddy,
                                                              const QString &privLevel,
                                                              std::optional<QString> colorLeft,
                                                              std::optional<QString> colorRight)
{
    QString iconType;
    if (isBuddy) {
        iconType = "star";
    } else if (privLevel.toLower() == "vip") {
        iconType = "pawn_vip";
    } else {
        iconType = "pawn";
    }

    QString arity = colorRight.has_value() ? "double" : "single";

    QString iconPath = QString("theme:usericons/%1_%2.svg").arg(iconType, arity);

    return loadAndColorSvg(iconPath, colorLeft.value(), colorRight, height);
}

QMap<QString, QIcon> UserLevelPixmapGenerator::iconCache;

QPixmap LockPixmapGenerator::generatePixmap(int height)
{

    int key = height;
    if (pmCache.contains(key))
        return pmCache.value(key);

    QPixmap pixmap = loadSvg("theme:icons/lock", QSize(height, height));
    pmCache.insert(key, pixmap);
    return pixmap;
}

QMap<int, QPixmap> LockPixmapGenerator::pmCache;

const QPixmap loadColorAdjustedPixmap(QString name)
{
    if (qApp->palette().windowText().color().lightness() > 200) {
        QImage img(name);
        img.invertPixels();
        QPixmap result;
        result.convertFromImage(img);
        return result;
    } else {
        return QPixmap(name);
    }
}
