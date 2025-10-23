/* preferences_dialog.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "preferences_dialog.h"
#include <ui_preferences_dialog.h>

#include "module_preferences_scroll_area.h"

#include <epan/prefs.h>
#include <epan/prefs-int.h>
#include <epan/decode_as.h>
#include <ui/language.h>
#include <ui/preference_utils.h>
#include <cfile.h>
#include <ui/commandline.h>
#include <ui/simple_dialog.h>
#include <ui/recent.h>
#include <main_window.h>
#include <extcap.h>

#include <ui/qt/utils/qt_ui_utils.h>
#include <ui/qt/utils/color_utils.h>

#include "main_application.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QMenu>
#include <QScrollBar>
#include <QUrl>

extern "C" {
// Callbacks prefs routines

static unsigned
module_prefs_unstash(module_t *module, void *data)
{
    unsigned int *must_redissect_p = static_cast<unsigned int *>(data);
    pref_unstash_data_t unstashed_data;

    unstashed_data.handle_decode_as = true;

    module->prefs_changed_flags = 0;        /* assume none of them changed */
    for (GList *pref_l = module->prefs; pref_l && pref_l->data; pref_l = gxx_list_next(pref_l)) {
        pref_t *pref = gxx_list_data(pref_t *, pref_l);

        if (prefs_is_preference_obsolete(pref) || prefs_get_type(pref) == PREF_STATIC_TEXT) continue;

        unstashed_data.module = module;
        pref_unstash(pref, &unstashed_data);
        commandline_options_drop(module->name, prefs_get_name(pref));
    }

    /* If any of them changed, indicate that we must redissect and refilter
       the current capture (if we have one), as the preference change
       could cause packets to be dissected differently. */
    *must_redissect_p |= module->prefs_changed_flags;

    if (prefs_module_has_submodules(module))
        return prefs_modules_foreach_submodules(module, module_prefs_unstash, data);

    return 0;     /* Keep unstashing. */
}

static unsigned
module_prefs_clean_stash(module_t *module, void *)
{
    for (GList *pref_l = module->prefs; pref_l && pref_l->data; pref_l = gxx_list_next(pref_l)) {
        pref_t *pref = gxx_list_data(pref_t *, pref_l);

        if (prefs_is_preference_obsolete(pref) || prefs_get_type(pref) == PREF_STATIC_TEXT) continue;

        pref_clean_stash(pref, Q_NULLPTR);
    }

    if (prefs_module_has_submodules(module))
        return prefs_modules_foreach_submodules(module, module_prefs_clean_stash, Q_NULLPTR);

    return 0;     /* Keep cleaning modules */
}

} // extern "C"

// Preference tree items
const int APPEARANCE_ITEM = 0;

//placeholder key to keep dynamically loaded preferences
static const char* MODULES_NAME = "Modules";

PreferencesDialog::PreferencesDialog(QWidget *parent) :
    GeometryStateDialog(parent),
    pd_ui_(new Ui::PreferencesDialog),
    model_(this),
    advancedPrefsModel_(this),
    advancedPrefsDelegate_(this),
    modulePrefsModel_(this)
{
    advancedPrefsModel_.setSourceModel(&model_);
    modulePrefsModel_.setSourceModel(&model_);
    saved_capture_no_extcap_ = prefs.capture_no_extcap;

    // Some classes depend on pref_ptr_to_pref_ so this MUST be called after
    // model_.populate().
    pd_ui_->setupUi(this);
    loadGeometry();

    setWindowTitle(mainApp->windowTitleString(tr("Preferences")));

    pd_ui_->advancedView->setModel(&advancedPrefsModel_);
    pd_ui_->advancedView->setItemDelegate(&advancedPrefsDelegate_);
    advancedPrefsModel_.setFirstColumnSpanned(pd_ui_->advancedView);

    pd_ui_->prefsView->setModel(&modulePrefsModel_);

    pd_ui_->splitter->setStretchFactor(0, 1);
    pd_ui_->splitter->setStretchFactor(1, 5);

    // The calculations done in showEvent to set the minimum size of the
    // protocol column mean that if we load the splitter state it will become
    // impossible to shrink the splitter below the width of the widest protocol
    // that initially fits, so don't do this unless we change showEvent.
    //loadSplitterState(pd_ui_->splitter);

    pd_ui_->prefsView->sortByColumn(ModulePrefsModel::colName, Qt::AscendingOrder);

    //Set the Appearance leaf to expanded
    pd_ui_->prefsView->setExpanded(modulePrefsModel_.index(APPEARANCE_ITEM, 0), true);


    // PreferencesPane, prefsView, and stackedWidget must all correspond to each other.
    prefs_pane_to_item_[PrefsModel::typeToString(PrefsModel::Appearance)] = pd_ui_->appearanceFrame;
    prefs_pane_to_item_[PrefsModel::typeToString(PrefsModel::Layout)] = pd_ui_->layoutFrame;
    prefs_pane_to_item_[PrefsModel::typeToString(PrefsModel::Columns)] = pd_ui_->columnFrame;
    prefs_pane_to_item_[PrefsModel::typeToString(PrefsModel::FontAndColors)] = pd_ui_->fontandcolorFrame;
    prefs_pane_to_item_[PrefsModel::typeToString(PrefsModel::Capture)] = pd_ui_->captureFrame;
    prefs_pane_to_item_[PrefsModel::typeToString(PrefsModel::Expert)] = pd_ui_->expertFrame;
    prefs_pane_to_item_[PrefsModel::typeToString(PrefsModel::FilterButtons)] = pd_ui_->filterExpressonsFrame;
    prefs_pane_to_item_[PrefsModel::typeToString(PrefsModel::RSAKeys)] = pd_ui_->rsaKeysFrame;
    prefs_pane_to_item_[PrefsModel::typeToString(PrefsModel::Advanced)] = pd_ui_->advancedFrame;
    prefs_pane_to_item_[MODULES_NAME] = NULL;

    pd_ui_->filterExpressonsFrame->setUat(uat_get_table_by_name("Display expressions"));
    pd_ui_->expertFrame->setUat(uat_get_table_by_name("Expert Info Severity Level Configuration"));

    connect(pd_ui_->prefsView, &PrefModuleTreeView::goToPane, this, &PreferencesDialog::selectPane);
    connect(pd_ui_->prefsView, &PrefModuleTreeView::expanded, this, &PreferencesDialog::resizeSplitter);
    connect(pd_ui_->prefsView, &PrefModuleTreeView::collapsed, this, &PreferencesDialog::resizeSplitter);

    /* Create a single-shot timer for debouncing calls to
     * updateSearchLineEdit() */
    searchLineEditTimer = new QTimer(this);
    searchLineEditTimer->setSingleShot(true);
    connect(searchLineEditTimer, &QTimer::timeout, this, &PreferencesDialog::updateSearchLineEdit);

    pd_ui_->advancedView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pd_ui_->advancedView, &QTreeView::customContextMenuRequested, this, &PreferencesDialog::handleCopyMenu);
}

PreferencesDialog::~PreferencesDialog()
{
    delete pd_ui_;
    delete searchLineEditTimer;
    prefs_modules_foreach_submodules(NULL, module_prefs_clean_stash, NULL);
}

void PreferencesDialog::setPane(const QString module_name)
{
    pd_ui_->prefsView->setPane(module_name);
}

void PreferencesDialog::enableAggregationOptions(bool enable)
{
    pd_ui_->captureFrame->enableAggregationOptions(enable);
}

void PreferencesDialog::keyPressEvent(QKeyEvent *event)
{
    if (pd_ui_->advancedSearchLineEdit->hasFocus() && (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)) {
        return; // Don't close window on enter/return.
    }

    GeometryStateDialog::keyPressEvent(event);
}

void PreferencesDialog::resizeSplitter()
{
    QList<int> sizes = pd_ui_->splitter->sizes();

    pd_ui_->prefsView->header()->setStretchLastSection(false);
    pd_ui_->prefsView->resizeColumnToContents(ModulePrefsModel::colName);

    int new_prefs_tree_width = pd_ui_->prefsView->columnWidth(ModulePrefsModel::colName);
    int scrollBarWidth = pd_ui_->prefsView->verticalScrollBar()->sizeHint().width();
    new_prefs_tree_width += scrollBarWidth;
    int border_width = pd_ui_->prefsView->style()->pixelMetric(QStyle::PM_DefaultFrameWidth);
    new_prefs_tree_width += 2 * border_width;

    sizes[1] = (sizes[0] + sizes[1]) - new_prefs_tree_width;
    sizes[0] = new_prefs_tree_width;

    pd_ui_->splitter->setSizes(sizes);
    pd_ui_->splitter->setStretchFactor(0, 1);
}

void PreferencesDialog::showEvent(QShowEvent *)
{
    resizeSplitter();

    pd_ui_->advancedView->expandAll();
    pd_ui_->advancedView->setSortingEnabled(true);
    pd_ui_->advancedView->sortByColumn(AdvancedPrefsModel::colName, Qt::AscendingOrder);

    int one_acw = fontMetrics().averageCharWidth();
    pd_ui_->advancedView->setColumnWidth(AdvancedPrefsModel::colName, one_acw * 35); // Don't let long items widen things too much
    pd_ui_->advancedView->resizeColumnToContents(AdvancedPrefsModel::colStatus);
    pd_ui_->advancedView->setColumnWidth(AdvancedPrefsModel::colType, one_acw * 22);
    pd_ui_->advancedView->setColumnWidth(AdvancedPrefsModel::colValue, one_acw * 30);
}

void PreferencesDialog::selectPane(QString pane)
{
    if (prefs_pane_to_item_.contains(pane)) {
        pd_ui_->stackedWidget->setCurrentWidget(prefs_pane_to_item_[pane]);
    } else {
        //If not found in prefs_pane_to_item_, it must be an individual module
        module_t* module = prefs_find_module(pane.toStdString().c_str());
        if (module != NULL) {
            QWidget* moduleWindow = prefs_pane_to_item_[MODULES_NAME];
            if (moduleWindow != NULL) {
                pd_ui_->stackedWidget->removeWidget(moduleWindow);
                delete moduleWindow;
            }

            moduleWindow = new ModulePreferencesScrollArea(module);
            prefs_pane_to_item_[MODULES_NAME] = moduleWindow;
            pd_ui_->stackedWidget->addWidget(moduleWindow);
            pd_ui_->stackedWidget->setCurrentWidget(moduleWindow);
        }
    }
}

void PreferencesDialog::handleCopyMenu(QPoint pos)
{
    QTreeView * tree = qobject_cast<QTreeView *>(sender());
    if (! tree)
        return;

    QModelIndex index = tree->indexAt(pos);
    if (! index.isValid())
        return;

    QMenu * menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    QAction * copyColumnAction = menu->addAction(tr("Copy"));
    copyColumnAction->setData(VariantPointer<QTreeView>::asQVariant(tree));
    connect(copyColumnAction, &QAction::triggered, this, &PreferencesDialog::copyActionTriggered);

    QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
    QAction * copyRowAction = menu->addAction(tr("Copy Row(s)", "", static_cast<int>(selectedRows.count())));
    copyRowAction->setData(VariantPointer<QTreeView>::asQVariant(tree));
    connect(copyRowAction, &QAction::triggered, this, &PreferencesDialog::copyRowActionTriggered);

    menu->popup(tree->viewport()->mapToGlobal(pos));
}

void PreferencesDialog::copyActionTriggered()
{
    QAction * sendingAction = qobject_cast<QAction *>(sender());
    if (! sendingAction)
        return;

    QTreeView * tree = VariantPointer<QTreeView>::asPtr(sendingAction->data());

    QModelIndexList selIndeces = tree->selectionModel()->selectedIndexes();

    int copyColumn = -1;
    QMenu * menu = qobject_cast<QMenu *>(sendingAction->parent());
    if (menu)
    {
        QPoint menuPosOnTable = tree->mapFromGlobal(QCursor::pos());
        QModelIndex clickedIndex = tree->indexAt(menuPosOnTable);
        if (clickedIndex.isValid())
            copyColumn = clickedIndex.column();
        if (copyColumn < 0)
            copyColumn = 0;
    }

    QString clipdata;
    if (selIndeces.count() > 0)
    {
        foreach(QModelIndex index, selIndeces)
        {
            if (index.column() == copyColumn)
            {
                QString data = tree->model()->data(index, Qt::DisplayRole).toString();
                clipdata.append(data.append("\n"));
            }
        }
    }

    QClipboard * clipBoard = QApplication::clipboard();
    clipBoard->setText(clipdata);
}

void PreferencesDialog::copyRowActionTriggered()
{
    QAction * sendingAction = qobject_cast<QAction *>(sender());
    if (! sendingAction)
        return;

    QTreeView * tree = VariantPointer<QTreeView>::asPtr(sendingAction->data());

    QModelIndexList selIndeces = tree->selectionModel()->selectedIndexes();

    QString clipdata;
    if (selIndeces.count() > 0)
    {
        int lastCol = tree->model()->columnCount() - 1;

        QStringList row;
        foreach(QModelIndex index, selIndeces)
        {
            row << tree->model()->data(index, Qt::DisplayRole).toString();
            if (index.column() < lastCol)
                continue;
            clipdata.append(row.join("\t\t").append("\n"));
            row.clear();
        }
    }

    QClipboard * clipBoard = QApplication::clipboard();
    clipBoard->setText(clipdata);
}

void PreferencesDialog::updateSearchLineEdit()
{
    advancedPrefsModel_.setFilter(searchLineEditText);
    /* If items are filtered out, then filtered back in, the tree remains collapsed
       Force an expansion */
    pd_ui_->advancedView->expandAll();
}

void PreferencesDialog::on_advancedSearchLineEdit_textEdited(const QString &text)
{
    /* As running pd_ui_->advancedView->expandAll() takes a noticeable amount
     * of time and so would introduce significant lag while typing a string
     * into the Search box, we instead debounce the call to
     * updateSearchLineEdit(), so that it doesn't run until a set amount of
     * time has elapsed with no updates to the Search field.
     *
     * If the user types something before the timer elapses, the timer restarts
     * the countdown.
     */
    searchLineEditText = text;
    searchLineEditTimer->start(prefs.gui_debounce_timer);
}

void PreferencesDialog::on_showChangedValuesCheckBox_toggled(bool checked)
{
    advancedPrefsModel_.setShowChangedValues(checked);
    /* If items are filtered out, then filtered back in, the tree remains collapsed
       Force an expansion */
    pd_ui_->advancedView->expandAll();
}

void PreferencesDialog::apply()
{
    char* err = NULL;
    unsigned int redissect_flags = 0;

    // XXX - We should validate preferences as the user changes them, not here.
    //       Some, but not all, of the preference controls validate the input,
    //       but they don't disable the OK/Apply button, and, what's worse, the
    //       "stashed" value is sometimes the last valid input, not, e.g., the
    //       input when the dialog was opened.
    // XXX - We're also too enthusiastic about setting must_redissect.
    prefs_modules_foreach_submodules(NULL, module_prefs_unstash, (void *)&redissect_flags);

    extcap_register_preferences();

    if (redissect_flags & PREF_EFFECT_GUI_LAYOUT) {
        // Layout type changed, reset sizes
        recent.gui_geometry_main_upper_pane = 0;
        recent.gui_geometry_main_lower_pane = 0;
        g_free(recent.gui_geometry_main_master_split);
        g_free(recent.gui_geometry_main_extra_split);
        recent.gui_geometry_main_master_split = NULL;
        recent.gui_geometry_main_extra_split = NULL;
    }

    pd_ui_->columnFrame->unstash();
    pd_ui_->filterExpressonsFrame->acceptChanges();
    pd_ui_->expertFrame->acceptChanges();
#ifdef HAVE_LIBGNUTLS
    pd_ui_->rsaKeysFrame->acceptChanges();
#endif

    //Filter expressions don't affect dissection, so there is no need to
    //send any events to that effect.  However, the app needs to know
    //about any button changes.
    mainApp->emitAppSignal(MainApplication::FilterExpressionsChanged);

    prefs_main_write();
    if (save_decode_as_entries(&err) < 0)
    {
        simple_dialog(ESD_TYPE_ERROR, ESD_BTN_OK, "%s", err);
        g_free(err);
    }

    write_language_prefs();
    mainApp->loadLanguage(QString(language));
    /*
     * Apply the protocol preferences first - "gui_prefs_apply()" could
     * cause redissection, and we have to make sure the protocol
     * preference changes have been fully applied.
     */
    prefs_apply_all();

    /* Fill in capture options with values from the preferences */
    prefs_to_capture_opts();
    mainApp->emitAppSignal(MainApplication::AggregationVisiblity);
    if (redissect_flags & PREF_EFFECT_AGGREGATION) {
        mainApp->emitAppSignal(MainApplication::AggregationChanged);
    }

    mainApp->setMonospaceFont(prefs.gui_font_name);

    if (redissect_flags & (PREF_EFFECT_GUI_COLOR)) {
        ColorUtils::setScheme(prefs.gui_color_scheme);
        mainApp->emitAppSignal(MainApplication::ColorsChanged);
    }

    if (redissect_flags & PREF_EFFECT_FIELDS) {
        mainApp->emitAppSignal(MainApplication::FieldsChanged);
    }

    if (redissect_flags & PREF_EFFECT_DISSECTION) {
        // Freeze the packet list early to avoid updating column data before doing a
        // full redissection. The packet list will be thawed when redissection is done.
        mainApp->emitAppSignal(MainApplication::FreezePacketList);

        /* Redissect all the packets, and re-evaluate the display filter. */
        mainApp->emitAppSignal(MainApplication::PacketDissectionChanged);
    }

    if (redissect_flags) {
        mainApp->emitAppSignal(MainApplication::PreferencesChanged);
    }

    if (redissect_flags & PREF_EFFECT_GUI_LAYOUT) {
        mainApp->emitAppSignal(MainApplication::RecentPreferencesRead);
    }

    if (prefs.capture_no_extcap != saved_capture_no_extcap_)
        mainApp->refreshLocalInterfaces();
}

void PreferencesDialog::on_buttonBox_accepted()
{
    apply();
    accept();
}

void PreferencesDialog::on_buttonBox_rejected()
{
    //handle frames that don't have their own OK/Cancel "buttons"
    pd_ui_->filterExpressonsFrame->rejectChanges();
    pd_ui_->expertFrame->rejectChanges();
#ifdef HAVE_LIBGNUTLS
    pd_ui_->rsaKeysFrame->rejectChanges();
#endif
    reject();
}

void PreferencesDialog::on_buttonBox_clicked(QAbstractButton *button)
{
    if (pd_ui_->buttonBox->buttonRole(button) == QDialogButtonBox::ApplyRole) {
        apply();
    }
}

void PreferencesDialog::on_buttonBox_helpRequested()
{
    QString help_page = modulePrefsModel_.data(pd_ui_->prefsView->currentIndex(), ModulePrefsModel::ModuleHelp).toString();
    if (!help_page.isEmpty()) {
        QString url = gchar_free_to_qstring(user_guide_url(help_page.toUtf8().constData()));
        QDesktopServices::openUrl(QUrl(QDir::fromNativeSeparators(url)));
    } else {
        // Generic help
        mainApp->helpTopicAction(HELP_PREFERENCES_DIALOG);
    }
}
