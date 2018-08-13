/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QtGui>
#include <QtWidgets>

#include "activitylistmodel.h"
#include "activitywidget.h"
#include "syncresult.h"
#include "logger.h"
#include "theme.h"
#include "folderman.h"
#include "syncfileitem.h"
#include "folder.h"
#include "openfilemanager.h"
#include "owncloudpropagator.h"
#include "account.h"
#include "accountstate.h"
#include "accountmanager.h"
#include "activityitemdelegate.h"
#include "QProgressIndicator.h"
#include "notificationconfirmjob.h"
#include "servernotificationhandler.h"
#include "theme.h"
#include "ocsjob.h"
#include "configfile.h"
#include "guiutility.h"
#include "socketapi.h"
#include "ui_activitywidget.h"
#include "syncengine.h"

#include <climits>

// time span in milliseconds which has to be between two
// refreshes of the notifications
#define NOTIFICATION_REQUEST_FREE_PERIOD 15000

namespace OCC {

ActivityWidget::ActivityWidget(AccountState *accountState, QWidget *parent)
    : QWidget(parent)
    , _ui(new Ui::ActivityWidget)
    , _notificationRequestsRunning(0)
    , _accountState(accountState)
    , _accept(tr("Accept"))
    , _remote_share("remote_share")
{
    _ui->setupUi(this);

// Adjust copyToClipboard() when making changes here!
#if defined(Q_OS_MAC)
    _ui->_activityList->setMinimumWidth(400);
#endif

    _model = new ActivityListModel(accountState, this);
    ActivityItemDelegate *delegate = new ActivityItemDelegate;
    delegate->setParent(this);
    _ui->_activityList->setItemDelegate(delegate);
    _ui->_activityList->setAlternatingRowColors(true);
    _ui->_activityList->setModel(_model);

    showLabels();

    connect(_model, &ActivityListModel::activityJobStatusCode,
        this, &ActivityWidget::slotAccountActivityStatus);

    _ui->_copyButton->setToolTip(tr("Copy the activity list to the clipboard."));
    connect(_ui->_copyButton, &QPushButton::clicked, this, &ActivityWidget::copyToClipboard);

    connect(_model, &QAbstractItemModel::rowsInserted, this, &ActivityWidget::rowsInserted);

    connect(delegate, &ActivityItemDelegate::primaryButtonClickedOnItemView, this, &ActivityWidget::slotPrimaryButtonClickedOnListView);
    connect(delegate, &ActivityItemDelegate::secondaryButtonClickedOnItemView, this, &ActivityWidget::slotSecondaryButtonClickedOnListView);
    connect(_ui->_activityList, &QListView::activated, this, &ActivityWidget::slotOpenFile);

    connect(ProgressDispatcher::instance(), &ProgressDispatcher::progressInfo,
        this, &ActivityWidget::slotProgressInfo);
    connect(ProgressDispatcher::instance(), &ProgressDispatcher::itemCompleted,
        this, &ActivityWidget::slotItemCompleted);
    connect(ProgressDispatcher::instance(), &ProgressDispatcher::syncError,
        this, &ActivityWidget::addError);

    _removeTimer.setInterval(1000);
}

ActivityWidget::~ActivityWidget()
{
    delete _ui;
}

void ActivityWidget::slotProgressInfo(const QString &folder, const ProgressInfo &progress)
{

// TODO: this is really not working
//    if (progress.status() == ProgressInfo::Done
//            || progress.status() == ProgressInfo::Reconcile) {
//        // Wipe all non-persistent entries - as well as the persistent ones
//        // in cases where a local discovery was done.
//        auto f = FolderMan::instance()->folder(folder);
//        if (!f)
//            return;
//        const auto &engine = f->syncEngine();
//        const auto style = engine.lastLocalDiscoveryStyle();
//        foreach (Activity activity, _model->errorsList()) {
//            if (activity._folder != folder){
//                continue;
//            }

//            if (style == LocalDiscoveryStyle::FilesystemOnly){
//                _model->removeActivityFromActivityList(activity);
//                continue;
//            }

//            if(activity._status == SyncFileItem::Conflict && !QFileInfo(f->path() + activity._file).exists()){
//                _model->removeActivityFromActivityList(activity);
//                continue;
//            }


//            if(activity._status == SyncFileItem::FileIgnored && !QFileInfo(f->path() + activity._file).exists()){
//                _model->removeActivityFromActivityList(activity);
//                continue;
//            }

//            if(!QFileInfo(f->path() + activity._file).exists()){
//                _model->removeActivityFromActivityList(activity);
//                continue;
//            }

//            auto path = QFileInfo(activity._file).dir().path().toUtf8();
//            if (path == ".")
//                path.clear();

//            if(engine.shouldDiscoverLocally(path))
//                _model->removeActivityFromActivityList(activity);
//        }

//    }

    if (progress.status() == ProgressInfo::Done) {
        // We keep track very well of pending conflicts.
        // Inform other components about them.
        QStringList conflicts;
        foreach (Activity activity, _model->errorsList()) {
            if (activity._folder == folder
                && activity._status == SyncFileItem::Conflict) {
                conflicts.append(activity._file);
            }
        }

        emit ProgressDispatcher::instance()->folderConflicts(folder, conflicts);
    }
}

void ActivityWidget::slotItemCompleted(const QString &folder, const SyncFileItemPtr &item){
    auto folderInstance = FolderMan::instance()->folder(folder);

    if (!folderInstance)
        return;

    // check if we are adding it to the right account and if it is useful information (protocol errors)
    if(folderInstance->accountState() == _accountState){
        qCWarning(lcActivity) << "Item " << item->_file << " retrieved resulted in " << item->_errorString;

        Activity activity;
        activity._type = Activity::SyncFileItemType;
        activity._status = item->_status;
        activity._dateTime = QDateTime::fromString(QDateTime::currentDateTime().toString(), Qt::ISODate);
        activity._subject = item->_errorString;
        activity._message = item->_originalFile;
        activity._link = folderInstance->accountState()->account()->url();
        activity._accName = folderInstance->accountState()->account()->displayName();
        activity._file = item->_file;
        activity._folder = folder;

        // add 'protocol error' to activity list
        _model->addErrorToActivityList(activity);
    }
}

void ActivityWidget::addError(const QString &folderAlias, const QString &message,
    ErrorCategory category)
{
    auto folderInstance = FolderMan::instance()->folder(folderAlias);
    if (!folderInstance)
        return;

    if(folderInstance->accountState() == _accountState){
        qCWarning(lcActivity) << "Item " << folderInstance->shortGuiLocalPath() << " retrieved resulted in " << message;

        Activity activity;
        activity._type = Activity::SyncResultType;
        activity._status = SyncResult::Error;
        activity._dateTime = QDateTime::fromString(QDateTime::currentDateTime().toString(), Qt::ISODate);
        activity._subject = message;
        activity._message = folderInstance->shortGuiLocalPath();
        activity._link = folderInstance->shortGuiLocalPath();
        activity._accName = folderInstance->accountState()->account()->displayName();
        activity._folder = folderAlias;


        if (category == ErrorCategory::InsufficientRemoteStorage) {
            ActivityLink link;
            link._label = tr("Retry all uploads");
            link._link = folderInstance->path();
            link._verb = "";
            link._isPrimary = true;
            activity._links.append(link);
        }

        // add 'other errors' to activity list
        _model->addErrorToActivityList(activity);
    }
}


void ActivityWidget::slotPrimaryButtonClickedOnListView(const QModelIndex &index){
    QUrl link = qvariant_cast<QString>(index.data(ActivityItemDelegate::LinkRole));
    QString objectType = index.data(ActivityItemDelegate::ObjectTypeRole).toString();
    if(!link.isEmpty()){
        qCWarning(lcActivity) << "Opening" << link.toString() <<  "in browser for Notification/Activity" << qvariant_cast<QString>(index.data(ActivityItemDelegate::ActionTextRole));
        Utility::openBrowser(link, this);
    } else if(objectType == _remote_share){
        QVariant customItem = index.data(ActivityItemDelegate::ActionsLinksRole).toList().first();
        ActivityLink actionLink = qvariant_cast<ActivityLink>(customItem);
        if(actionLink._label == _accept){
            qCWarning(lcActivity) << objectType <<  "action" << actionLink._label << "for" << qvariant_cast<QString>(index.data(ActivityItemDelegate::ActionTextRole));
            const QString accountName = index.data(ActivityItemDelegate::AccountRole).toString();
            slotSendNotificationRequest(accountName, actionLink._link, actionLink._verb, index.row());
        } else {
            qCWarning(lcActivity) << "Failed: " << objectType <<  "action" << actionLink._label << "for" << qvariant_cast<QString>(index.data(ActivityItemDelegate::ActionTextRole));
        }
    }
}

void ActivityWidget::slotSecondaryButtonClickedOnListView(const QModelIndex &index){
    QList<QVariant> customList = index.data(ActivityItemDelegate::ActionsLinksRole).toList();
    QString objectType = index.data(ActivityItemDelegate::ObjectTypeRole).toString();

    QList<ActivityLink> actionLinks;
    foreach(QVariant customItem, customList){
        actionLinks << qvariant_cast<ActivityLink>(customItem);
    }

    if(objectType == _remote_share && actionLinks.first()._label == _accept)
        actionLinks.removeFirst();

    if(qvariant_cast<Activity::Type>(index.data(ActivityItemDelegate::ActionRole)) == Activity::Type::NotificationType){
        const QString accountName = index.data(ActivityItemDelegate::AccountRole).toString();
        if(actionLinks.size() == 1){
            if(actionLinks.at(0)._verb == "DELETE"){
                qCWarning(lcActivity) << "Dismissing Notification/Activity" << qvariant_cast<QString>(index.data(ActivityItemDelegate::ActionTextRole));
                slotSendNotificationRequest(index.data(ActivityItemDelegate::AccountRole).toString(), actionLinks.at(0)._link, actionLinks.at(0)._verb, index.row());
            }
        } else if(actionLinks.size() > 1){
            QMenu menu;
            qCWarning(lcActivity) << "Displaying menu for Notification/Activity" << qvariant_cast<QString>(index.data(ActivityItemDelegate::ActionTextRole));
            foreach (ActivityLink actionLink, actionLinks) {
                QAction *menuAction = new QAction(actionLink._label, &menu);
                connect(menuAction, &QAction::triggered, this, [this, index, accountName, actionLink] {
                    this->slotSendNotificationRequest(accountName, actionLink._link, actionLink._verb, index.row());
                });
                menu.addAction(menuAction);
            }
            menu.exec(QCursor::pos());
        }
    }

    Activity::Type activityType = qvariant_cast<Activity::Type>(index.data(ActivityItemDelegate::ActionRole));
    if(activityType == Activity::Type::SyncFileItemType || activityType == Activity::Type::SyncResultType)
        slotOpenFile(index);
}

void ActivityWidget::slotNotificationRequestFinished(int statusCode)
{
    int row = sender()->property("activityRow").toInt();

    // the ocs API returns stat code 100 or 200 inside the xml if it succeeded.
    if (statusCode != OCS_SUCCESS_STATUS_CODE && statusCode != OCS_SUCCESS_STATUS_CODE_V2) {
        qCWarning(lcActivity) << "Notification Request to Server failed, leave notification visible.";
    } else {
       // to do use the model to rebuild the list or remove the item
        qCWarning(lcActivity) << "Notification Request to Server successed, rebuilding list.";
       _model->removeActivityFromActivityList(row);
    }
}

void ActivityWidget::slotRefreshActivities()
{
    _model->slotRefreshActivity();
}

void ActivityWidget::slotRefreshNotifications()
{
    // start a server notification handler if no notification requests
    // are running
    if (_notificationRequestsRunning == 0) {
        ServerNotificationHandler *snh = new ServerNotificationHandler(_accountState);
        connect(snh, &ServerNotificationHandler::newNotificationList,
            this, &ActivityWidget::slotBuildNotificationDisplay);

        snh->slotFetchNotifications();
    } else {
        qCWarning(lcActivity) << "Notification request counter not zero.";
    }
}

void ActivityWidget::slotRemoveAccount()
{
    _model->slotRemoveAccount();
}

void ActivityWidget::showLabels()
{
    QString t = tr("Server Activities");
    t.clear();
    QSetIterator<QString> i(_accountsWithoutActivities);
    while (i.hasNext()) {
        t.append(tr("<br/>Account %1 does not have activities enabled.").arg(i.next()));
    }
    _ui->_bottomLabel->setTextFormat(Qt::RichText);
    _ui->_bottomLabel->setText(t);
}

void ActivityWidget::slotAccountActivityStatus(int statusCode)
{
    if (!(_accountState && _accountState->account())) {
        return;
    }
    if (statusCode == 999) {
        _accountsWithoutActivities.insert(_accountState->account()->displayName());
    } else {
        _accountsWithoutActivities.remove(_accountState->account()->displayName());
    }

    checkActivityWidgetVisibility();
    showLabels();
}

// FIXME: Reused from protocol widget. Move over to utilities.
QString ActivityWidget::timeString(QDateTime dt, QLocale::FormatType format) const
{
    const QLocale loc = QLocale::system();
    QString dtFormat = loc.dateTimeFormat(format);
    static const QRegExp re("(HH|H|hh|h):mm(?!:s)");
    dtFormat.replace(re, "\\1:mm:ss");
    return loc.toString(dt, dtFormat);
}

void ActivityWidget::storeActivityList(QTextStream &ts)
{
    ActivityList activities = _model->activityList();

    foreach (Activity activity, activities) {
        ts << right
           // account name
           << qSetFieldWidth(activity._accName.length())
           << activity._accName
           // separator
           << qSetFieldWidth(2) << " - "

           // date and time
           << qSetFieldWidth(activity._dateTime.toString().length())
           << activity._dateTime.toString()
           // separator
           << qSetFieldWidth(2) << " - "

           // fileq
           << qSetFieldWidth(activity._file.length())
           << activity._file
           // separator
           << qSetFieldWidth(2) << " - "

           // subject
           << qSetFieldWidth(activity._subject.length())
           << activity._subject
           // separator
           << qSetFieldWidth(2) << " - "

          // message
          << qSetFieldWidth(activity._message.length())
          << activity._message
          << endl;
    }
}

void ActivityWidget::checkActivityWidgetVisibility()
{
    int accountCount = AccountManager::instance()->accounts().count();
    bool hasAccountsWithActivity =
        _accountsWithoutActivities.count() != accountCount;

    _ui->_activityList->setVisible(hasAccountsWithActivity);

    emit hideActivityTab(!hasAccountsWithActivity);
}

void ActivityWidget::slotOpenFile(QModelIndex indx)
{
    qCDebug(lcActivity) << indx.isValid() << indx.data(ActivityItemDelegate::PathRole).toString() << QFile::exists(indx.data(ActivityItemDelegate::PathRole).toString());
    if (indx.isValid()) {
        QString fullPath = indx.data(ActivityItemDelegate::PathRole).toString();
        if(!fullPath.isEmpty()){
            if (QFile::exists(fullPath)) {
                showInFileManager(fullPath);
            }
        }
    }
}

// GUI: Display the notifications.
// All notifications in list are coming from the same account
// but in the _widgetForNotifId hash widgets for all accounts are
// collected.
void ActivityWidget::slotBuildNotificationDisplay(const ActivityList &list)
{
    QString listAccountName;

    // Whether a new notification was added to the list
    bool newNotificationShown = false;

    foreach (auto activity, list) {
        if (_blacklistedNotifications.contains(activity)) {
            qCInfo(lcActivity) << "Activity in blacklist, skip";
            continue;
        }

        // remember the list account name for the strayCat handling below.
        listAccountName = activity._accName;

        // handle gui logs. In order to NOT annoy the user with every fetching of the
        // notifications the notification id is stored in a Set. Only if an id
        // is not in the set, it qualifies for guiLog.
        // Important: The _guiLoggedNotifications set must be wiped regularly which
        // will repeat the gui log.

        // after one hour, clear the gui log notification store
        if (_guiLogTimer.elapsed() > 60 * 60 * 1000) {
            _guiLoggedNotifications.clear();
        }

        if (!_guiLoggedNotifications.contains(activity._id)) {
            newNotificationShown = true;
            _guiLoggedNotifications.insert(activity._id);

            // Assemble a tray notification for the NEW notification
            ConfigFile cfg;
            if(cfg.optionalServerNotifications()){
                if(AccountManager::instance()->accounts().count() == 1){
                    emit guiLog(activity._subject, "");
                } else {
                    emit guiLog(activity._subject, activity._accName);
                }
            }

            _model->addNotificationToActivityList(activity);
        }
    }

    // restart the gui log timer now that we show a new notification
    if(newNotificationShown)
        _guiLogTimer.start();
}

void ActivityWidget::slotSendNotificationRequest(const QString &accountName, const QString &link, const QByteArray &verb, int row)
{
    qCInfo(lcActivity) << "Server Notification Request " << verb << link << "on account" << accountName;

    const QStringList validVerbs = QStringList() << "GET"
                                                 << "PUT"
                                                 << "POST"
                                                 << "DELETE";

    if (validVerbs.contains(verb)) {
        AccountStatePtr acc = AccountManager::instance()->account(accountName);
        if (acc) {
            NotificationConfirmJob *job = new NotificationConfirmJob(acc->account());
            QUrl l(link);
            job->setLinkAndVerb(l, verb);
            job->setProperty("activityRow", QVariant::fromValue(row));
            connect(job, &AbstractNetworkJob::networkError,
                this, &ActivityWidget::slotNotifyNetworkError);
            connect(job, &NotificationConfirmJob::jobFinished,
                this, &ActivityWidget::slotNotifyServerFinished);
            job->start();

            // count the number of running notification requests. If this member var
            // is larger than zero, no new fetching of notifications is started
            _notificationRequestsRunning++;
        }
    } else {
        qCWarning(lcActivity) << "Notification Links: Invalid verb:" << verb;
    }
}

void ActivityWidget::endNotificationRequest(int replyCode)
{
    _notificationRequestsRunning--;
    slotNotificationRequestFinished(replyCode);
}

void ActivityWidget::slotNotifyNetworkError(QNetworkReply *reply)
{
    NotificationConfirmJob *job = qobject_cast<NotificationConfirmJob *>(sender());
    if (!job) {
        return;
    }

    int resultCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    endNotificationRequest(resultCode);
    qCWarning(lcActivity) << "Server notify job failed with code " << resultCode;
}

void ActivityWidget::slotNotifyServerFinished(const QString &reply, int replyCode)
{
    NotificationConfirmJob *job = qobject_cast<NotificationConfirmJob *>(sender());
    if (!job) {
        return;
    }

    endNotificationRequest(replyCode);
    qCInfo(lcActivity) << "Server Notification reply code" << replyCode << reply;
}

/* ==================================================================== */

ActivitySettings::ActivitySettings(AccountState *accountState, QWidget *parent)
    : QWidget(parent)
    , _accountState(accountState)
{
    _vbox = new QVBoxLayout(this);
    setLayout(_vbox);

    _activityWidget = new ActivityWidget(_accountState, this);

    _vbox->insertWidget(1, _activityWidget);
    connect(_activityWidget, &ActivityWidget::copyToClipboard, this, &ActivitySettings::slotCopyToClipboard);
    connect(_activityWidget, &ActivityWidget::guiLog, this, &ActivitySettings::guiLog);
    connect(&_notificationCheckTimer, &QTimer::timeout,
        this, &ActivitySettings::slotRegularNotificationCheck);

    // Add a progress indicator to spin if the acitivity list is updated.
    _progressIndicator = new QProgressIndicator(this);

    // connect a model signal to stop the animation
    connect(_activityWidget, &ActivityWidget::rowsInserted, _progressIndicator, &QProgressIndicator::stopAnimation);
    connect(_activityWidget, &ActivityWidget::rowsInserted, this, &ActivitySettings::slotDisplayActivities);
}

void ActivitySettings::slotDisplayActivities(){
   _vbox->removeWidget(_progressIndicator);
}

void ActivitySettings::setNotificationRefreshInterval(std::chrono::milliseconds interval)
{
    qCDebug(lcActivity) << "Starting Notification refresh timer with " << interval.count() / 1000 << " sec interval";
    _notificationCheckTimer.start(interval.count());
}

void ActivitySettings::slotCopyToClipboard()
{
    QString text;
    QTextStream ts(&text);

    QString message;

    _activityWidget->storeActivityList(ts);
    message = tr("The server activity and notifications list has been copied to the clipboard.");

    QApplication::clipboard()->setText(text);

    emit guiLog(tr("Copied to clipboard"), message);
}

void ActivitySettings::slotRemoveAccount()
{
    _activityWidget->slotRemoveAccount();
}

void ActivitySettings::slotRefresh()
{
    // QElapsedTimer isn't actually constructed as invalid.
    if (!_timeSinceLastCheck.contains(_accountState)) {
        _timeSinceLastCheck[_accountState].invalidate();
    }
    QElapsedTimer &timer = _timeSinceLastCheck[_accountState];

    // Fetch Activities only if visible and if last check is longer than 15 secs ago
    if (timer.isValid() && timer.elapsed() < NOTIFICATION_REQUEST_FREE_PERIOD) {
        qCDebug(lcActivity) << "Do not check as last check is only secs ago: " << timer.elapsed() / 1000;
        return;
    }
    if (_accountState && _accountState->isConnected()) {
        if (isVisible() || !timer.isValid()) {
            _vbox->insertWidget(0, _progressIndicator);
            _vbox->setAlignment(_progressIndicator, Qt::AlignHCenter);
            _progressIndicator->startAnimation();
            _activityWidget->slotRefreshActivities();
        }
        _activityWidget->slotRefreshNotifications();
        timer.start();
    }
}

void ActivitySettings::slotRegularNotificationCheck()
{
    slotRefresh();
}

bool ActivitySettings::event(QEvent *e)
{
    if (e->type() == QEvent::Show) {
        slotRefresh();
    }
    return QWidget::event(e);
}

ActivitySettings::~ActivitySettings()
{
}
}
