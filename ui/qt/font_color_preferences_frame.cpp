/* font_color_preferences_frame.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>

#include <ui/qt/utils/qt_ui_utils.h>

#include "font_color_preferences_frame.h"
#include <ui/qt/models/pref_models.h>
#include <ui_font_color_preferences_frame.h>
#include <ui/qt/utils/color_utils.h>
#include "main_application.h"
#include "wsutil/array.h"

#include <functional>
#include <QFontDialog>
#include <QColorDialog>

#include <epan/prefs-int.h>

//: These are pangrams. Feel free to replace with nonsense text that spans your alphabet.
//: https://en.wikipedia.org/wiki/Pangram
static const char *font_pangrams_[] = {
    QT_TRANSLATE_NOOP("FontColorPreferencesFrame", "Example GIF query packets have jumbo window sizes"),
    QT_TRANSLATE_NOOP("FontColorPreferencesFrame", "Lazy badgers move unique waxy jellyfish packets")
};
const int num_font_pangrams_ = array_length(font_pangrams_);

FontColorPreferencesFrame::FontColorPreferencesFrame(QWidget *parent) :
    QFrame(parent),
    ui(new Ui::FontColorPreferencesFrame),
    colorSchemeComboBox_(nullptr)
{
    ui->setupUi(this);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0) && (defined(Q_OS_WIN) || defined(Q_OS_MAC))
    // This doesn't work under most Linux platforms.
    // KDE doesn't have color scheme as a separate option from theme,
    // and requires changing the theme. GTK4/libadwaita does have color
    // scheme as a separate option, but Qt doesn't fully work with it.
    // On Linux should this be invisible, disabled, or have a warning
    // label that it probably won't work?
    QHBoxLayout *colorSchemeLayout = new QHBoxLayout();
    QLabel *colorSchemeLabel = new QLabel(tr("Color Scheme:"));
    colorSchemeLayout->addWidget(colorSchemeLabel);
    colorSchemeComboBox_ = new QComboBox();
    colorSchemeComboBox_->addItem(tr("System Default"), COLOR_SCHEME_DEFAULT);
    colorSchemeComboBox_->addItem(tr("Light Mode"), COLOR_SCHEME_LIGHT);
    colorSchemeComboBox_->addItem(tr("Dark Mode"), COLOR_SCHEME_DARK);
    connect(colorSchemeComboBox_, &QComboBox::currentIndexChanged,
        this, &FontColorPreferencesFrame::colorSchemeIndexChanged);
    colorSchemeLayout->addWidget(colorSchemeComboBox_);
    colorSchemeLayout->addStretch();

    ui->verticalLayout->insertLayout(ui->verticalLayout->indexOf(ui->colorsLabel), colorSchemeLayout);
#endif

    pref_color_scheme_ = prefFromPrefPtr(&prefs.gui_color_scheme);
    pref_qt_gui_font_name_ = prefFromPrefPtr(&prefs.gui_font_name);
    pref_active_fg_ = prefFromPrefPtr(&prefs.gui_active_fg);
    pref_active_bg_ = prefFromPrefPtr(&prefs.gui_active_bg);
    pref_active_style_ = prefFromPrefPtr(&prefs.gui_active_style);
    pref_inactive_fg_ = prefFromPrefPtr(&prefs.gui_inactive_fg);
    pref_inactive_bg_ = prefFromPrefPtr(&prefs.gui_inactive_bg);
    pref_inactive_style_ = prefFromPrefPtr(&prefs.gui_inactive_style);
    pref_marked_fg_ = prefFromPrefPtr(&prefs.gui_marked_fg);
    pref_marked_bg_ = prefFromPrefPtr(&prefs.gui_marked_bg);
    pref_ignored_fg_ = prefFromPrefPtr(&prefs.gui_ignored_fg);
    pref_ignored_bg_ = prefFromPrefPtr(&prefs.gui_ignored_bg);
    pref_client_fg_ = prefFromPrefPtr(&prefs.st_client_fg);
    pref_client_bg_ = prefFromPrefPtr(&prefs.st_client_bg);
    pref_server_fg_ = prefFromPrefPtr(&prefs.st_server_fg);
    pref_server_bg_ = prefFromPrefPtr(&prefs.st_server_bg);
    pref_valid_fg_ = prefFromPrefPtr(&prefs.gui_filter_valid_fg);
    pref_valid_bg_ = prefFromPrefPtr(&prefs.gui_filter_valid_bg);
    pref_invalid_fg_ = prefFromPrefPtr(&prefs.gui_filter_invalid_fg);
    pref_invalid_bg_ = prefFromPrefPtr(&prefs.gui_filter_invalid_bg);
    pref_deprecated_fg_ = prefFromPrefPtr(&prefs.gui_filter_deprecated_fg);
    pref_deprecated_bg_ = prefFromPrefPtr(&prefs.gui_filter_deprecated_bg);

    cur_font_.fromString(prefs_get_string_value(pref_qt_gui_font_name_, pref_stashed));

}

FontColorPreferencesFrame::~FontColorPreferencesFrame()
{
    delete ui;
}

void FontColorPreferencesFrame::showEvent(QShowEvent *)
{
    GRand *rand_state = g_rand_new();
    QString pangram = QStringLiteral("%1 0123456789").arg(font_pangrams_[g_rand_int_range(rand_state, 0, num_font_pangrams_)]);
    ui->fontSampleLineEdit->setText(pangram);
    ui->fontSampleLineEdit->setCursorPosition(0);
    ui->fontSampleLineEdit->setMinimumWidth(mainApp->monospaceTextSize(pangram.toUtf8().constData()) + mainApp->monospaceTextSize(" "));
    g_rand_free(rand_state);

    updateWidgets();
}

void FontColorPreferencesFrame::updateWidgets()
{
    int      colorstyle;
    QColor   foreground;
    QColor   background1;
    QColor   background2;
    QPalette default_pal;

    int margin = style()->pixelMetric(QStyle::PM_LayoutLeftMargin);

    ui->fontPushButton->setText(
        cur_font_.family() + " " + cur_font_.styleName() + " " +
        QString::number(cur_font_.pointSizeF(), 'f', 1));

    QString line_edit_ss = QStringLiteral("QLineEdit { margin-left: %1px; }").arg(margin);
    ui->fontSampleLineEdit->setStyleSheet(line_edit_ss);
    ui->fontSampleLineEdit->setFont(cur_font_);

    QString color_button_ss =
        "QPushButton {"
        "  border: 1px solid palette(Dark);"
        "  background-color: %1;"
        "  margin-left: %2px;"
        "}";
    QString sample_text_ss =
        "QLineEdit {"
        "  border: 1px solid palette(Dark);"
        "  color: %1;"
        "  background-color: %2;"
        "}";
    QString sample_text_ex_ss =
        "QLineEdit {"
        "  border: 1px solid palette(Dark);"
        "  color: %1;"
        "  background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1 stop: 0 %3, stop: 0.5 %2, stop: 1 %3);"
        "}";

    if (colorSchemeComboBox_) {
        colorSchemeComboBox_->setCurrentIndex(colorSchemeComboBox_->findData(prefs_get_enum_value(pref_color_scheme_, pref_stashed)));
    }

    //
    // Sample active selected item
    //
    colorstyle = prefs_get_enum_value(pref_active_style_, pref_stashed);

    // Make foreground and background colors
    switch (colorstyle)
    {
    case COLOR_STYLE_DEFAULT:
        default_pal = QApplication::palette();
        default_pal.setCurrentColorGroup(QPalette::Active);

        foreground  = default_pal.highlightedText().color();
        background1 = default_pal.highlight().color();
        break;

    case COLOR_STYLE_FLAT:
        foreground  = ColorUtils::fromColorT(prefs_get_color_value(pref_active_fg_, pref_stashed));
        background1 = ColorUtils::fromColorT(prefs_get_color_value(pref_active_bg_, pref_stashed));
        break;

    case COLOR_STYLE_GRADIENT:
        foreground  = ColorUtils::fromColorT(prefs_get_color_value(pref_active_fg_, pref_stashed));
        background1 = ColorUtils::fromColorT(prefs_get_color_value(pref_active_bg_, pref_stashed));
        background2 = QColor::fromRgb(ColorUtils::alphaBlend(foreground, background1, COLOR_STYLE_ALPHA));
        break;
    }

    ui->activeFGPushButton->setStyleSheet(color_button_ss.arg(foreground.name()).arg(margin));
    ui->activeBGPushButton->setStyleSheet(color_button_ss.arg(background1.name()).arg(0));
    if (colorstyle == COLOR_STYLE_GRADIENT) {
        ui->activeSampleLineEdit->setStyleSheet(sample_text_ex_ss.arg(
                                                foreground.name(),
                                                background1.name(),
                                                background2.name()));
    } else {
        ui->activeSampleLineEdit->setStyleSheet(sample_text_ss.arg(
                                                foreground.name(),
                                                background1.name()));
    }
    ui->activeSampleLineEdit->setFont(cur_font_);
    ui->activeStyleComboBox->setCurrentIndex(prefs_get_enum_value(pref_active_style_, pref_stashed));

    // Show or hide the widgets
    ui->activeFGPushButton->setVisible(colorstyle != COLOR_STYLE_DEFAULT);
    ui->activeBGPushButton->setVisible(colorstyle != COLOR_STYLE_DEFAULT);

    //
    // Sample inactive selected item
    //
    colorstyle = prefs_get_enum_value(pref_inactive_style_, pref_stashed);

    // Make foreground and background colors
    switch (colorstyle)
    {
    case COLOR_STYLE_DEFAULT:
        default_pal = QApplication::palette();
        default_pal.setCurrentColorGroup(QPalette::Inactive);

        foreground  = default_pal.highlightedText().color();
        background1 = default_pal.highlight().color();
        break;

    case COLOR_STYLE_FLAT:
        foreground  = ColorUtils::fromColorT(prefs_get_color_value(pref_inactive_fg_, pref_stashed));
        background1 = ColorUtils::fromColorT(prefs_get_color_value(pref_inactive_bg_, pref_stashed));
        break;

    case COLOR_STYLE_GRADIENT:
        foreground  = ColorUtils::fromColorT(prefs_get_color_value(pref_inactive_fg_, pref_stashed));
        background1 = ColorUtils::fromColorT(prefs_get_color_value(pref_inactive_bg_, pref_stashed));
        background2 = QColor::fromRgb(ColorUtils::alphaBlend(foreground, background1, COLOR_STYLE_ALPHA));
        break;
    }

    ui->inactiveFGPushButton->setStyleSheet(color_button_ss.arg(foreground.name()).arg(margin));
    ui->inactiveBGPushButton->setStyleSheet(color_button_ss.arg(background1.name()).arg(0));
    if (colorstyle == COLOR_STYLE_GRADIENT) {
        ui->inactiveSampleLineEdit->setStyleSheet(sample_text_ex_ss.arg(
                                                  foreground.name(),
                                                  background1.name(),
                                                  background2.name()));
    } else {
        ui->inactiveSampleLineEdit->setStyleSheet(sample_text_ss.arg(
                                                  foreground.name(),
                                                  background1.name()));
    }
    ui->inactiveSampleLineEdit->setFont(cur_font_);
    ui->inactiveStyleComboBox->setCurrentIndex(prefs_get_enum_value(pref_inactive_style_, pref_stashed));

    // Show or hide the widgets
    ui->inactiveFGPushButton->setVisible(colorstyle != COLOR_STYLE_DEFAULT);
    ui->inactiveBGPushButton->setVisible(colorstyle != COLOR_STYLE_DEFAULT);

    //
    // Sample marked packet text
    //
    ui->markedFGPushButton->setStyleSheet(color_button_ss.arg(
                                              ColorUtils::fromColorT(prefs_get_color_value(pref_marked_fg_, pref_stashed)).name())
                                              .arg(margin));
    ui->markedBGPushButton->setStyleSheet(color_button_ss.arg(
                                              ColorUtils::fromColorT(prefs_get_color_value(pref_marked_bg_, pref_stashed)).name())
                                              .arg(0));
    ui->markedSampleLineEdit->setStyleSheet(sample_text_ss.arg(
                                                ColorUtils::fromColorT(prefs_get_color_value(pref_marked_fg_, pref_stashed)).name(),
                                                ColorUtils::fromColorT(prefs_get_color_value(pref_marked_bg_, pref_stashed)).name()));
    ui->markedSampleLineEdit->setFont(cur_font_);

    //
    // Sample ignored packet text
    //
    ui->ignoredFGPushButton->setStyleSheet(color_button_ss.arg(
                                               ColorUtils::fromColorT(prefs_get_color_value(pref_ignored_fg_, pref_stashed)).name())
                                               .arg(margin));
    ui->ignoredBGPushButton->setStyleSheet(color_button_ss.arg(
                                               ColorUtils::fromColorT(prefs_get_color_value(pref_ignored_bg_, pref_stashed)).name())
                                               .arg(0));
    ui->ignoredSampleLineEdit->setStyleSheet(sample_text_ss.arg(
                                                 ColorUtils::fromColorT(prefs_get_color_value(pref_ignored_fg_, pref_stashed)).name(),
                                                 ColorUtils::fromColorT(prefs_get_color_value(pref_ignored_bg_, pref_stashed)).name()));
    ui->ignoredSampleLineEdit->setFont(cur_font_);

    //
    // Sample "Follow Stream" client text
    //
    ui->clientFGPushButton->setStyleSheet(color_button_ss.arg(
                                              ColorUtils::fromColorT(prefs_get_color_value(pref_client_fg_, pref_stashed)).name())
                                              .arg(margin));
    ui->clientBGPushButton->setStyleSheet(color_button_ss.arg(
                                              ColorUtils::fromColorT(prefs_get_color_value(pref_client_bg_, pref_stashed)).name())
                                              .arg(0));
    ui->clientSampleLineEdit->setStyleSheet(sample_text_ss.arg(
                                                ColorUtils::fromColorT(prefs_get_color_value(pref_client_fg_, pref_stashed)).name(),
                                                ColorUtils::fromColorT(prefs_get_color_value(pref_client_bg_, pref_stashed)).name()));
    ui->clientSampleLineEdit->setFont(cur_font_);

    //
    // Sample "Follow Stream" server text
    //
    ui->serverFGPushButton->setStyleSheet(color_button_ss.arg(
                                              ColorUtils::fromColorT(prefs_get_color_value(pref_server_fg_, pref_stashed)).name())
                                              .arg(margin));
    ui->serverBGPushButton->setStyleSheet(color_button_ss.arg(
                                              ColorUtils::fromColorT(prefs_get_color_value(pref_server_bg_, pref_stashed)).name())
                                              .arg(0));
    ui->serverSampleLineEdit->setStyleSheet(sample_text_ss.arg(
                                                ColorUtils::fromColorT(prefs_get_color_value(pref_server_fg_, pref_stashed)).name(),
                                                ColorUtils::fromColorT(prefs_get_color_value(pref_server_bg_, pref_stashed)).name()));
    ui->serverSampleLineEdit->setFont(cur_font_);

    //
    // Sample valid filter
    //
    QColor ss_bg = ColorUtils::fromColorT(prefs_get_color_value(pref_valid_bg_, pref_stashed));
    QColor ss_fg = ColorUtils::fromColorT(prefs_get_color_value(pref_valid_fg_, pref_stashed));
    ui->validFilterBGPushButton->setStyleSheet(color_button_ss.arg(
                                                   ColorUtils::fromColorT(prefs_get_color_value(pref_valid_bg_, pref_stashed)).name())
                                                   .arg(0));
    ui->validFilterFGPushButton->setStyleSheet(color_button_ss.arg(
                                                 ColorUtils::fromColorT(prefs_get_color_value(pref_valid_fg_, pref_stashed)).name())
                                                   .arg(margin));
    ui->validFilterSampleLineEdit->setStyleSheet(sample_text_ss.arg(
                                                     ss_fg.name(),
                                                     ss_bg.name()));

    //
    // Sample invalid filter
    //
    ss_bg = ColorUtils::fromColorT(prefs_get_color_value(pref_invalid_bg_, pref_stashed));
    ss_fg = ColorUtils::fromColorT(prefs_get_color_value(pref_invalid_fg_, pref_stashed));
    ui->invalidFilterBGPushButton->setStyleSheet(color_button_ss.arg(
                                                     ColorUtils::fromColorT(prefs_get_color_value(pref_invalid_bg_, pref_stashed)).name())
                                                     .arg(0));
    ui->invalidFilterFGPushButton->setStyleSheet(color_button_ss.arg(
                                                     ColorUtils::fromColorT(prefs_get_color_value(pref_invalid_fg_, pref_stashed)).name())
                                                       .arg(margin));
    ui->invalidFilterSampleLineEdit->setStyleSheet(sample_text_ss.arg(
                                                       ss_fg.name(),
                                                       ss_bg.name()));

    //
    // Sample warning filter
    //
    ss_bg = ColorUtils::fromColorT(prefs_get_color_value(pref_deprecated_bg_, pref_stashed));
    ss_fg = ColorUtils::fromColorT(prefs_get_color_value(pref_deprecated_fg_, pref_stashed));
    ui->deprecatedFilterBGPushButton->setStyleSheet(color_button_ss.arg(
                                                        ColorUtils::fromColorT(prefs_get_color_value(pref_deprecated_bg_, pref_stashed)).name())
                                                        .arg(0));
    ui->deprecatedFilterFGPushButton->setStyleSheet(color_button_ss.arg(
                                                        ColorUtils::fromColorT(prefs_get_color_value(pref_deprecated_fg_, pref_stashed)).name())
                                                        .arg(margin));
    ui->deprecatedFilterSampleLineEdit->setStyleSheet(sample_text_ss.arg(
                                                          ss_fg.name(),
                                                          ss_bg.name()));
}

void FontColorPreferencesFrame::changeColor(pref_t *pref)
{
    QColorDialog *color_dlg = new QColorDialog();
    color_t* color = prefs_get_color_value(pref, pref_stashed);

    color_dlg->setCurrentColor(QColor(
                                  color->red >> 8,
                                  color->green >> 8,
                                  color->blue >> 8
                                  ));

    connect(color_dlg, &QColorDialog::colorSelected, std::bind(&FontColorPreferencesFrame::colorChanged, this, pref, std::placeholders::_1));
    color_dlg->setWindowModality(Qt::ApplicationModal);
    color_dlg->setAttribute(Qt::WA_DeleteOnClose);
    color_dlg->show();
}

void FontColorPreferencesFrame::colorChanged(pref_t *pref, const QColor &cc)
{
    color_t new_color;
    new_color.red = cc.red() << 8 | cc.red();
    new_color.green = cc.green() << 8 | cc.green();
    new_color.blue = cc.blue() << 8 | cc.blue();
    prefs_set_color_value(pref, new_color, pref_stashed);
    updateWidgets();
}

void FontColorPreferencesFrame::colorSchemeIndexChanged(int)
{
    if (colorSchemeComboBox_) {
        prefs_set_enum_value(pref_color_scheme_, colorSchemeComboBox_->currentData().toInt(), pref_stashed);
        // COLOR_SCHEME_DEFAULT is 0 so we don't need to check failure
        updateWidgets();
    }
}

void FontColorPreferencesFrame::on_fontPushButton_clicked()
{
    bool ok;
    QFont new_font = QFontDialog::getFont(&ok, cur_font_, this, mainApp->windowTitleString(tr("Font")));
    if (ok) {
        prefs_set_string_value(pref_qt_gui_font_name_, new_font.toString().toStdString().c_str(), pref_stashed);
        cur_font_ = new_font;
        updateWidgets();
    }
}

void FontColorPreferencesFrame::on_activeFGPushButton_clicked()
{
    changeColor(pref_active_fg_);
}

void FontColorPreferencesFrame::on_activeBGPushButton_clicked()
{
    changeColor(pref_active_bg_);
}

void FontColorPreferencesFrame::on_activeStyleComboBox_currentIndexChanged(int index)
{
    prefs_set_enum_value(pref_active_style_, index, pref_stashed);
    updateWidgets();
}


void FontColorPreferencesFrame::on_inactiveFGPushButton_clicked()
{
    changeColor(pref_inactive_fg_);
}

void FontColorPreferencesFrame::on_inactiveBGPushButton_clicked()
{
    changeColor(pref_inactive_bg_);
}

void FontColorPreferencesFrame::on_inactiveStyleComboBox_currentIndexChanged(int index)
{
    prefs_set_enum_value(pref_inactive_style_, index, pref_stashed);
    updateWidgets();
}

void FontColorPreferencesFrame::on_markedFGPushButton_clicked()
{
    changeColor(pref_marked_fg_);
}

void FontColorPreferencesFrame::on_markedBGPushButton_clicked()
{
    changeColor(pref_marked_bg_);
}

void FontColorPreferencesFrame::on_ignoredFGPushButton_clicked()
{
    changeColor(pref_ignored_fg_);
}

void FontColorPreferencesFrame::on_ignoredBGPushButton_clicked()
{
    changeColor(pref_ignored_bg_);
}

void FontColorPreferencesFrame::on_clientFGPushButton_clicked()
{
    changeColor(pref_client_fg_);
}

void FontColorPreferencesFrame::on_clientBGPushButton_clicked()
{
    changeColor(pref_client_bg_);
}

void FontColorPreferencesFrame::on_serverFGPushButton_clicked()
{
    changeColor(pref_server_fg_);
}

void FontColorPreferencesFrame::on_serverBGPushButton_clicked()
{
    changeColor(pref_server_bg_);
}

void FontColorPreferencesFrame::on_validFilterBGPushButton_clicked()
{
    changeColor(pref_valid_bg_);
}

void FontColorPreferencesFrame::on_validFilterFGPushButton_clicked()
{
    changeColor(pref_valid_fg_);
}

void FontColorPreferencesFrame::on_invalidFilterBGPushButton_clicked()
{
    changeColor(pref_invalid_bg_);
}

void FontColorPreferencesFrame::on_invalidFilterFGPushButton_clicked()
{
    changeColor(pref_invalid_fg_);
}

void FontColorPreferencesFrame::on_deprecatedFilterBGPushButton_clicked()
{
    changeColor(pref_deprecated_bg_);
}

void FontColorPreferencesFrame::on_deprecatedFilterFGPushButton_clicked()
{
    changeColor(pref_deprecated_fg_);
}
