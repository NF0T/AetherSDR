#include "PropDashboardDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QScrollArea>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonArray>

namespace AetherSDR {

static const QString kGroupStyle =
    "QGroupBox { color: #00b4d8; font-weight: bold; border: 1px solid #304050; "
    "border-radius: 4px; margin-top: 8px; padding-top: 14px; } "
    "QGroupBox::title { subcontrol-origin: margin; left: 10px; }";

static const QString kLabelStyle = "QLabel { color: #8090a0; font-size: 11px; }";
static const QString kValueStyle = "QLabel { color: #c8d8e8; font-weight: bold; font-size: 12px; }";
static const QString kSepStyle = "QLabel { color: #304050; font-size: 1px; }";

QString PropDashboardDialog::kpColor(double kp)
{
    if (kp >= 5.0) return "#ff4040";
    if (kp >= 3.0) return "#c0a000";
    return "#00c040";
}

PropDashboardDialog::PropDashboardDialog(PropForecastClient* client, QWidget* parent)
    : QDialog(parent), m_client(client)
{
    setWindowTitle("HF Propagation Dashboard");
    setMinimumSize(720, 560);
    resize(780, 620);

    m_nam = new QNetworkAccessManager(this);

    auto* root = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll);

    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* mainLayout = new QVBoxLayout(content);
    mainLayout->setContentsMargins(8, 4, 8, 4);
    mainLayout->setSpacing(6);

    auto makeVal = [](const QString& init = "\xe2\x80\x94") {
        auto* l = new QLabel(init);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        l->setStyleSheet(kValueStyle);
        return l;
    };

    // ── Current Conditions (single row) ──────────────────────────────────
    m_currentCondLabel = new QLabel;
    m_currentCondLabel->setAlignment(Qt::AlignCenter);
    m_currentCondLabel->setStyleSheet(
        "QLabel { color: #c8d8e8; font-weight: bold; font-size: 14px; "
        "background: #0a1a28; border: 1px solid #304050; border-radius: 4px; "
        "padding: 4px 12px; }");
    mainLayout->addWidget(m_currentCondLabel);

    // ── Row 1: Images (left) + Combined 3-Day Table (right) ──────────────
    auto* row1 = new QHBoxLayout;
    row1->setSpacing(8);

    // Images column
    {
        auto* imgCol = new QVBoxLayout;
        imgCol->setSpacing(4);

        auto* solarGroup = new QGroupBox("Solar (SDO/AIA)");
        solarGroup->setStyleSheet(kGroupStyle);
        auto* sv = new QVBoxLayout(solarGroup);
        sv->setContentsMargins(4, 14, 4, 4);
        m_solarImage = new QLabel;
        m_solarImage->setFixedSize(160, 160);
        m_solarImage->setAlignment(Qt::AlignCenter);
        m_solarImage->setStyleSheet("QLabel { background: #000; border-radius: 4px; }");
        m_solarImage->setCursor(Qt::PointingHandCursor);
        m_solarImage->setToolTip("Click to cycle through solar images");
        m_solarImage->setText("Loading...");
        m_solarImage->installEventFilter(this);
        sv->addWidget(m_solarImage, 0, Qt::AlignCenter);
        m_solarTypeLabel = new QLabel("Corona (193\xc3\x85)");
        m_solarTypeLabel->setAlignment(Qt::AlignCenter);
        m_solarTypeLabel->setStyleSheet("QLabel { color: #8090a0; font-size: 10px; }");
        sv->addWidget(m_solarTypeLabel);
        imgCol->addWidget(solarGroup);

        auto* lunarGroup = new QGroupBox("Lunar");
        lunarGroup->setStyleSheet(kGroupStyle);
        auto* lv = new QVBoxLayout(lunarGroup);
        lv->setContentsMargins(4, 14, 4, 4);
        m_lunarImage = new QLabel;
        m_lunarImage->setFixedSize(100, 100);
        m_lunarImage->setAlignment(Qt::AlignCenter);
        m_lunarImage->setStyleSheet("QLabel { background: #000; border-radius: 4px; }");
        m_lunarImage->setText("Loading...");
        lv->addWidget(m_lunarImage, 0, Qt::AlignCenter);
        m_lunarPhaseLabel = new QLabel;
        m_lunarPhaseLabel->setAlignment(Qt::AlignCenter);
        m_lunarPhaseLabel->setStyleSheet("QLabel { color: #c0a000; font-size: 10px; }");
        lv->addWidget(m_lunarPhaseLabel);
        imgCol->addWidget(lunarGroup);

        imgCol->addStretch();
        row1->addLayout(imgCol);
    }

    // Combined 3-day table: Kp forecast + Blackout + Radiation
    {
        auto* group = new QGroupBox("3-Day Forecast");
        group->setStyleSheet(kGroupStyle);
        auto* g = new QGridLayout(group);
        g->setVerticalSpacing(1);
        g->setHorizontalSpacing(6);

        auto* timeHdr = new QLabel("UTC");
        timeHdr->setStyleSheet(kLabelStyle + " font-weight: bold;");
        g->addWidget(timeHdr, 0, 0);
        for (int d = 0; d < 3; ++d) {
            m_dayHeaders[d] = new QLabel("\xe2\x80\x94");
            m_dayHeaders[d]->setAlignment(Qt::AlignCenter);
            m_dayHeaders[d]->setStyleSheet(kLabelStyle + " font-weight: bold;");
            g->addWidget(m_dayHeaders[d], 0, d + 1);
        }

        // Kp rows
        static const char* periods[] = {
            "00-03", "03-06", "06-09", "09-12",
            "12-15", "15-18", "18-21", "21-00"
        };
        for (int p = 0; p < 8; ++p) {
            auto* lbl = new QLabel(periods[p]);
            lbl->setStyleSheet(kLabelStyle);
            g->addWidget(lbl, p + 1, 0);
            for (int d = 0; d < 3; ++d) {
                m_kpCells[d][p] = new QLabel("\xe2\x80\x94");
                m_kpCells[d][p]->setAlignment(Qt::AlignCenter);
                m_kpCells[d][p]->setStyleSheet(kValueStyle + " font-size: 11px;");
                g->addWidget(m_kpCells[d][p], p + 1, d + 1);
            }
        }

        // Max Kp
        int row = 9;
        auto* maxLbl = new QLabel("Max Kp:");
        maxLbl->setStyleSheet(kLabelStyle + " font-weight: bold;");
        g->addWidget(maxLbl, row, 0);
        for (int d = 0; d < 3; ++d) {
            m_maxKpLabels[d] = new QLabel("\xe2\x80\x94");
            m_maxKpLabels[d]->setAlignment(Qt::AlignCenter);
            m_maxKpLabels[d]->setStyleSheet(kValueStyle + " font-size: 12px;");
            g->addWidget(m_maxKpLabels[d], row, d + 1);
        }

        // Separator
        ++row;
        auto* sep = new QLabel;
        sep->setFixedHeight(1);
        sep->setStyleSheet("QLabel { background: #304050; }");
        g->addWidget(sep, row, 0, 1, 4);

        // R1-R2 Blackout
        ++row;
        auto* r1Lbl = new QLabel("R1-R2:");
        r1Lbl->setStyleSheet(kLabelStyle);
        g->addWidget(r1Lbl, row, 0);
        for (int d = 0; d < 3; ++d) {
            m_r1r2Labels[d] = makeVal();
            g->addWidget(m_r1r2Labels[d], row, d + 1);
        }

        // R3+ Blackout
        ++row;
        auto* r3Lbl = new QLabel("R3+:");
        r3Lbl->setStyleSheet(kLabelStyle);
        g->addWidget(r3Lbl, row, 0);
        for (int d = 0; d < 3; ++d) {
            m_r3Labels[d] = makeVal();
            g->addWidget(m_r3Labels[d], row, d + 1);
        }

        // S1+ Radiation
        ++row;
        auto* s1Lbl = new QLabel("S1+:");
        s1Lbl->setStyleSheet(kLabelStyle);
        g->addWidget(s1Lbl, row, 0);
        for (int d = 0; d < 3; ++d) {
            m_s1Labels[d] = makeVal();
            g->addWidget(m_s1Labels[d], row, d + 1);
        }

        // Rationale
        ++row;
        m_rationale = new QLabel;
        m_rationale->setWordWrap(true);
        m_rationale->setStyleSheet("QLabel { color: #6080a0; font-size: 10px; }");
        g->addWidget(m_rationale, row, 0, 1, 4);

        row1->addWidget(group, 1);
    }

    mainLayout->addLayout(row1);

    // ── Row 2: Band Conditions + VHF ─────────────────────────────────────
    auto* row2 = new QHBoxLayout;
    row2->setSpacing(8);

    // HF Band Conditions
    {
        auto* group = new QGroupBox("HF Band Conditions");
        group->setStyleSheet(kGroupStyle);
        auto* g = new QGridLayout(group);
        g->setVerticalSpacing(3);
        g->setHorizontalSpacing(10);

        static const char* bandNames[] = {"80m-40m", "30m-20m", "17m-15m", "12m-10m"};

        auto* dayHdr = new QLabel("Day");
        dayHdr->setAlignment(Qt::AlignCenter);
        dayHdr->setStyleSheet(kLabelStyle + " font-weight: bold;");
        g->addWidget(dayHdr, 0, 1);
        auto* nightHdr = new QLabel("Night");
        nightHdr->setAlignment(Qt::AlignCenter);
        nightHdr->setStyleSheet(kLabelStyle + " font-weight: bold;");
        g->addWidget(nightHdr, 0, 2);

        for (int i = 0; i < 4; ++i) {
            auto* lbl = new QLabel(bandNames[i]);
            lbl->setStyleSheet(kLabelStyle);
            g->addWidget(lbl, i + 1, 0);
            m_bandDayLabels[i] = makeVal();
            g->addWidget(m_bandDayLabels[i], i + 1, 1);
            m_bandNightLabels[i] = makeVal();
            g->addWidget(m_bandNightLabels[i], i + 1, 2);
        }

        int r = 5;
        auto addRow = [&](const char* name, QLabel*& label) {
            auto* lbl = new QLabel(name);
            lbl->setStyleSheet(kLabelStyle);
            g->addWidget(lbl, r, 0);
            label = makeVal();
            g->addWidget(label, r++, 1, 1, 2);
        };
        addRow("X-Ray:", m_xrayLabel);
        addRow("Solar Wind:", m_windLabel);
        addRow("Geomagnetic:", m_geoFieldLabel);
        addRow("Noise:", m_noiseLabel);

        row2->addWidget(group);
    }

    // VHF Conditions
    {
        auto* group = new QGroupBox("VHF Conditions");
        group->setStyleSheet(kGroupStyle);
        auto* g = new QGridLayout(group);
        g->setVerticalSpacing(4);

        auto* aurLbl = new QLabel("Aurora:");
        aurLbl->setStyleSheet(kLabelStyle);
        g->addWidget(aurLbl, 0, 0);
        m_auroraLabel = makeVal();
        g->addWidget(m_auroraLabel, 0, 1);

        auto* esNaLbl = new QLabel("E-Skip NA:");
        esNaLbl->setStyleSheet(kLabelStyle);
        g->addWidget(esNaLbl, 1, 0);
        m_esNaLabel = makeVal();
        g->addWidget(m_esNaLabel, 1, 1);

        auto* esEuLbl = new QLabel("E-Skip EU:");
        esEuLbl->setStyleSheet(kLabelStyle);
        g->addWidget(esEuLbl, 2, 0);
        m_esEuLabel = makeVal();
        g->addWidget(m_esEuLabel, 2, 1);

        row2->addWidget(group);
    }

    mainLayout->addLayout(row2);

    // ── Close ────────────────────────────────────────────────────────────
    auto* closeBtn = new QPushButton("Close");
    closeBtn->setFixedWidth(80);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    // Wire updates
    connect(m_client, &PropForecastClient::detailUpdated,
            this, &PropDashboardDialog::onDetailUpdated);
    connect(m_client, &PropForecastClient::forecastUpdated,
            this, [this](const PropForecast&) { refresh(); });

    refresh();
    m_client->fetchDetail();
    fetchImages();

    m_refreshTimer.setInterval(30 * 60 * 1000);
    connect(&m_refreshTimer, &QTimer::timeout, this, [this]() {
        m_client->fetchDetail();
        fetchImages();
    });
    m_refreshTimer.start();
}

void PropDashboardDialog::fetchImages()
{
    fetchSolarImage();

    // Lunar phase
    QString ts = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm");
    QNetworkRequest lReq{QUrl{QString("https://svs.gsfc.nasa.gov/api/dialamoon/%1").arg(ts)}};
    lReq.setHeader(QNetworkRequest::UserAgentHeader, "AetherSDR");
    auto* lr = m_nam->get(lReq);
    connect(lr, &QNetworkReply::finished, this, [this, lr]() {
        lr->deleteLater();
        if (lr->error() != QNetworkReply::NoError) { m_lunarImage->setText("N/A"); return; }
        QJsonDocument doc = QJsonDocument::fromJson(lr->readAll());
        if (!doc.isObject()) return;
        QJsonObject obj = doc.object();

        QString phase = obj["curphase"].toString();
        int illum = static_cast<int>(obj["fracillum"].toString().toDouble());
        m_lunarPhaseLabel->setText(QString("%1\n%2% illuminated").arg(phase).arg(illum));

        QString imgUrl = obj["image"].toObject()["url"].toString();
        if (imgUrl.isEmpty()) return;
        QNetworkRequest ir{QUrl{imgUrl}};
        ir.setHeader(QNetworkRequest::UserAgentHeader, "AetherSDR");
        auto* imgR = m_nam->get(ir);
        connect(imgR, &QNetworkReply::finished, this, [this, imgR]() {
            imgR->deleteLater();
            if (imgR->error() == QNetworkReply::NoError) {
                QPixmap pm;
                if (pm.loadFromData(imgR->readAll()))
                    m_lunarImage->setPixmap(pm.scaled(100, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
        });
    });
}

void PropDashboardDialog::refresh()
{
    const PropForecast fc = m_client->lastForecast();
    const PropForecastDetail& det = m_client->lastDetail();

    QString cond;
    if (fc.kIndex >= 0)
        cond += QString("<span style='color:%1'>K%2</span>").arg(kpColor(fc.kIndex)).arg(fc.kIndex, 0, 'f', 2);
    if (fc.aIndex >= 0)
        cond += QString("  A%1").arg(fc.aIndex);
    if (fc.sfi > 0)
        cond += QString("  SFI %1").arg(fc.sfi);
    if (det.sunspotNumber >= 0)
        cond += QString("  SSN %1").arg(det.sunspotNumber);
    if (!det.xrayClass.isEmpty())
        cond += QString("  X-Ray %1").arg(det.xrayClass);
    m_currentCondLabel->setText(cond);
}

void PropDashboardDialog::onDetailUpdated(const PropForecastDetail& det)
{
    for (int d = 0; d < 3; ++d)
        m_dayHeaders[d]->setText(det.dayLabels[d]);

    for (int d = 0; d < 3; ++d) {
        for (int p = 0; p < 8; ++p) {
            double kp = det.kpForecast[d][p];
            m_kpCells[d][p]->setText(QString::number(kp, 'f', 2));
            m_kpCells[d][p]->setStyleSheet(
                QString("QLabel { color: %1; font-weight: bold; font-size: 11px; }").arg(kpColor(kp)));
        }
        m_maxKpLabels[d]->setText(QString::number(det.maxKp[d], 'f', 2));
        m_maxKpLabels[d]->setStyleSheet(
            QString("QLabel { color: %1; font-weight: bold; font-size: 12px; }").arg(kpColor(det.maxKp[d])));
    }

    for (int d = 0; d < 3; ++d) {
        m_r1r2Labels[d]->setText(QString("%1%").arg(det.blackoutR1R2[d]));
        m_r3Labels[d]->setText(QString("%1%").arg(det.blackoutR3[d]));
        m_s1Labels[d]->setText(QString("%1%").arg(det.radiationS1[d]));
    }

    QString rat;
    if (!det.geomagRationale.isEmpty())
        rat += det.geomagRationale;
    if (!det.blackoutRationale.isEmpty()) {
        if (!rat.isEmpty()) rat += " ";
        rat += det.blackoutRationale;
    }
    m_rationale->setText(rat);

    // Band conditions
    auto bandColor = [](const QString& c) -> QString {
        if (c == "Good") return "#00c040";
        if (c == "Fair") return "#c0a000";
        return (c == "Poor") ? "#ff4040" : "#8090a0";
    };
    for (int i = 0; i < 4; ++i) {
        if (!det.bandDay[i].isEmpty()) {
            m_bandDayLabels[i]->setText(det.bandDay[i]);
            m_bandDayLabels[i]->setStyleSheet(
                QString("QLabel { color: %1; font-weight: bold; }").arg(bandColor(det.bandDay[i])));
        }
        if (!det.bandNight[i].isEmpty()) {
            m_bandNightLabels[i]->setText(det.bandNight[i]);
            m_bandNightLabels[i]->setStyleSheet(
                QString("QLabel { color: %1; font-weight: bold; }").arg(bandColor(det.bandNight[i])));
        }
    }

    if (!det.xrayClass.isEmpty()) m_xrayLabel->setText(det.xrayClass);
    if (det.solarWind > 0) m_windLabel->setText(QString("%1 km/s").arg(det.solarWind, 0, 'f', 1));
    if (!det.geomagField.isEmpty()) m_geoFieldLabel->setText(det.geomagField);
    if (!det.signalNoise.isEmpty()) m_noiseLabel->setText(det.signalNoise);

    auto vhfColor = [](const QString& c) { return c.contains("Open") ? "#00c040" : "#ff4040"; };
    if (!det.vhfAurora.isEmpty()) {
        m_auroraLabel->setText(det.vhfAurora);
        m_auroraLabel->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; }").arg(vhfColor(det.vhfAurora)));
    }
    if (!det.esNorthAmerica.isEmpty()) {
        m_esNaLabel->setText(det.esNorthAmerica);
        m_esNaLabel->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; }").arg(vhfColor(det.esNorthAmerica)));
    }
    if (!det.esEurope.isEmpty()) {
        m_esEuLabel->setText(det.esEurope);
        m_esEuLabel->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; }").arg(vhfColor(det.esEurope)));
    }

    refresh();
}

// ── Solar image cycling ──────────────────────────────────────────────────

struct SolarImageType {
    const char* code;   // SDO URL suffix
    const char* label;  // Display name
};

static constexpr SolarImageType kSolarTypes[] = {
    {"0193", "Corona (193\xc3\x85)"},
    {"0304", "Chromosphere (304\xc3\x85)"},
    {"0171", "Quiet Corona (171\xc3\x85)"},
    {"0094", "Flaring (94\xc3\x85)"},
    {"HMIIC", "Visible (HMI)"},
};
static constexpr int kSolarTypeCount = 5;

void PropDashboardDialog::fetchSolarImage()
{
    const auto& type = kSolarTypes[m_solarTypeIndex % kSolarTypeCount];
    QString url = QString("https://sdo.gsfc.nasa.gov/assets/img/latest/latest_256_%1.jpg").arg(type.code);

    m_solarTypeLabel->setText(type.label);
    m_solarImage->setText("Loading...");

    QNetworkRequest req{QUrl{url}};
    req.setHeader(QNetworkRequest::UserAgentHeader, "AetherSDR");
    auto* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            QPixmap pm;
            if (pm.loadFromData(reply->readAll()))
                m_solarImage->setPixmap(pm.scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            m_solarImage->setText("Unavailable");
        }
    });
}

void PropDashboardDialog::cycleSolarImage()
{
    m_solarTypeIndex = (m_solarTypeIndex + 1) % kSolarTypeCount;
    fetchSolarImage();
}

bool PropDashboardDialog::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_solarImage && ev->type() == QEvent::MouseButtonPress) {
        cycleSolarImage();
        return true;
    }
    return QDialog::eventFilter(obj, ev);
}

} // namespace AetherSDR
