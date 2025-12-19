#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStringListModel>
#include <QStyleFactory>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <QtEnvironmentVariables>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>

#include <signal.h>
#include <unistd.h>

#include <QtDebug>

#include "gauge.h"
#include "notifications.h"
#include "qiq.h"

static QRegularExpression whitespace("[;&|[:space:]]+"); //[^\\\\]*
static Qiq *gs_qiq = nullptr;
#ifndef Q_OS_WIN
static void sighandler(int signum) {
    switch(signum) {
//        case SIGABRT:
        case SIGINT:
//        case SIGSEGV:
        case SIGTERM:
            if (gs_qiq)
                gs_qiq->writeHistory();
            break;
        default:
            break;
    }
    raise(signum);
}
#endif

void DBusReceptor::ActionInvoked(uint id, QString action_key) {
    if (id == m_id) {
        printf("%s\n", action_key.toLocal8Bit().data());
        quit();
    }
}

void DBusReceptor::NotificationClosed(uint id, uint reason) {
    if (id == m_id) {
        printf("%u\n", reason);
        quit();
    }
}


int notify(const QStringList &args) {
    if (args.isEmpty()) {
        qDebug() << "You need to provide at least a summary";
        return -1;
    }

    QDBusInterface notifications( "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications" );
    QList<QVariant> vl;
    if (args.at(0).startsWith("close=")) {
        vl << args.at(0).mid(6).toUInt();
        notifications.callWithArgumentList(QDBus::NoBlock, "CloseNotification", vl);
        return 0;
    }

    bool waitForClose = false;
    bool waitForAction = false;
    QString summary = args.at(0);
    QVariantMap hints;
    uint id = 0;
    QString appName, appIcon, body;
    int timeout = 0;
    QStringList actions;

    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == "transient")
            hints["transient"] = true;
        else if (args.at(i) == "resident")
            hints["resident"] = true;
        else if (args.at(i) == "wait")
            waitForClose = true;
        else if (args.at(i) == "countdown")
            hints["countdown"] = true;
        else {
            const QString key = args.at(i).section('=',0,0);
            const QString value = args.at(i).section('=',1,-1);
            if (key == "id")
                id = value.toInt();
            else if (key == "body")
                body = value;
            else if (key == "appname")
                appName = value;
            else if (key == "urgency") // low, normal, critical
                hints["urgency"] = value;
            else if (key == "timeout")
                timeout = value.toInt();
            else if (key == "icon")
                appIcon = value;
            else if (key == "image")
                hints["image-path"] = value;
            else if (key == "category")
                hints["category"] = value;
            else if (key == "actions") {
                actions = value.split(',');
                waitForClose = waitForAction = true;
            }
        }
    }
    vl << appName << id << appIcon << summary << body << actions << hints << timeout;
    QDBusReply<uint> reply = notifications.callWithArgumentList(QDBus::Block, "Notify", vl);
    if (reply.isValid()) {
        if (waitForClose) {
            DBusReceptor a(reply.value());
            if (waitForAction)
                QDBusConnection::sessionBus().connect("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                            "org.freedesktop.Notifications", "ActionInvoked", &a, SLOT(ActionInvoked(uint, QString)));
            // unconditionally also to allow handling whatever happens
            QDBusConnection::sessionBus().connect("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                            "org.freedesktop.Notifications", "NotificationClosed", &a, SLOT(NotificationClosed(uint, uint)));
            return a.exec();
        }
        printf("%d\n", reply.value());
        return 0;
    }
    return -1;
}

int main (int argc, char **argv)
{
    QString command;
    QStringList parameters;
    bool isDaemon = false;
    if (argc > 1) {
        QStringList validCommands = QString("daemon\nfilter\nreconfigure\ntoggle\nnotify").split('\n');
        command = QString::fromLocal8Bit(argv[1]);
        if (command == "qiq_daemon") {
            isDaemon = true;
            command = QString();
        }
        if (!isDaemon && !validCommands.contains(command)) {
            qWarning() << "No such command" << command << "\nValid commands:\n" << validCommands;
            return 1;
        }
        for (int i = 2; i < argc; ++i)
            parameters << QString::fromLocal8Bit(argv[i]);
    }
    QDBusConnectionInterface *session = QDBusConnection::sessionBus().interface();
    if (!isDaemon && command.isEmpty() && session->isServiceRegistered("org.qiq.qiq"))
        command = "toggle";
    if (!command.isEmpty()) {
        if (command == "notify")
            return notify(parameters);
        if (!session->isServiceRegistered("org.qiq.qiq")) {
            if (!QProcess::startDetached(argv[0], QStringList() << "qiq_daemon"))
                return 1;
            QElapsedTimer time;
            time.start();
            while (!session->isServiceRegistered("org.qiq.qiq")) {
                if (time.elapsed() > 5000) {
                    qWarning() << "Could not contact nor start daemon, aborting";
                    return 1;
                }
                QThread::msleep(50);
            }
        }
        QDBusInterface qiq( "org.qiq.qiq", "/", "org.qiq.qiq" );
        if (command == "toggle") {
            qiq.call(QDBus::NoBlock, "toggle");
            return 0;
        }
        if (command == "reconfigure") {
            qiq.call(QDBus::NoBlock, "reconfigure");
            return 0;
        }
        if (command == "filter") {
            QList<QVariant> vl;
            for (const QString &s : parameters)
                vl << s;
            if (parameters.size() > 1 && parameters.at(1).startsWith("%print")) {
                QDBusReply<QString> reply = qiq.callWithArgumentList(QDBus::Block, "filter", vl);
                if (reply.isValid())
                    printf("%s\n", reply.value().toLocal8Bit().data());
            } else {
                qiq.callWithArgumentList(QDBus::NoBlock, "filter", vl);
            }
            return 0;
        }
        return 0;
    }

    QStyle *style = QStyleFactory::create("Fusion"); // take some cotrol to allow predictable style sheets
    QApplication::setStyle(style);
    QApplication a(argc, argv);
    QApplication::setStyle(style);
    QCoreApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles, true);
    Qiq *q = new Qiq;
    if (isDaemon)
        q->hide();
#ifndef Q_OS_WIN
    gs_qiq = q;
    struct sigaction sa;
    sa.sa_handler = sighandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART|SA_RESETHAND;
    for (int signum : {/* SIGABRT,SIGSEGV, */SIGINT,SIGTERM}) {
        if (sigaction(signum, &sa, NULL) == -1)
            qDebug() << "no signal handling for" << signum;
    }
#endif
    return a.exec();
}


Qiq::Qiq() : QStackedWidget() {
    QDBusConnection::sessionBus().registerService("org.qiq.qiq");
    QDBusConnection::sessionBus().registerObject("/", this);
    new DBusAdaptor(this);
//    qEnvironmentVariable(const char *varName, const QString &defaultValue)
    setWindowFlags(Qt::BypassWindowManagerHint/* |Qt::FramelessWindowHint|Qt::WindowStaysOnTopHint */);
    addWidget(m_status = new QWidget);

    m_notifications = new Notifications;

    m_external = nullptr;
    m_cmdCompleted = nullptr;
    m_historySaver = nullptr;

    addWidget(m_list = new QListView);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(true);

//    m_list->setAutoFillBackground(false);
//    m_list->viewport()->setAutoFillBackground(false);
    m_lastVisibleRow = -1;
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->viewport()->setFocusPolicy(Qt::NoFocus);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_list, &QAbstractItemView::clicked, [=](const QModelIndex &idx) {
        m_list->setCurrentIndex(idx);
        insertToken();
        runInput();
    });

    addWidget(m_disp = new QTextBrowser);
    m_disp->setFrameShape(QFrame::NoFrame);
    m_disp->setFocusPolicy(Qt::NoFocus);
//    m_disp->setFocusPolicy(Qt::ClickFocus);
    m_input = new QLineEdit(this);
    connect(this, &QStackedWidget::currentChanged, [=]() {
        adjustGeometry();
        m_input->raise();
        if (currentWidget() == m_list)
            connect(m_input, &QLineEdit::textEdited, this, &Qiq::filterInput, Qt::UniqueConnection);
        else
            disconnect(m_input, &QLineEdit::textEdited, this, &Qiq::filterInput);
    });

    reconfigure();

    if (!m_historyPath.isEmpty()) {
        QFile f(m_historyPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            m_history = QString::fromUtf8(f.readAll()).split('\n');
        } else {
            qDebug() << "could not open" << m_historyPath << "for reading";
        }
    }

    m_input->setGeometry(0,0,0,m_input->height());
    m_input->setFrame(QFrame::NoFrame);
    m_input->setAutoFillBackground(false);
    m_input->setAlignment(Qt::AlignCenter);
    QFont fnt = font();
    fnt.setPointSize(2*fnt.pointSize());
    m_input->setFont(fnt);
    m_input->setFocus();
    m_input->hide();
    QPalette pal = m_input->palette();
    pal.setColor(m_input->backgroundRole(), QColor(0,0,0,192));
    pal.setColor(m_input->foregroundRole(), QColor(255,255,255));
    pal.setColor(QPalette::Highlight, QColor(255,255,255,192));
    pal.setColor(QPalette::HighlightedText, QColor(0,0,0));
    m_input->setPalette(pal);
    m_input->installEventFilter(this);
    makeApplicationModel();
    const QStringList paths = qEnvironmentVariable("PATH").split(':');
    QStringList binaries;
    for (const QString &s : paths)
        binaries.append(QDir(s).entryList(QDir::Files|QDir::Executable));
    binaries.removeDuplicates();
    binaries.sort();
    m_bins = new QStringListModel(binaries);
    m_files = new QFileSystemModel(this);
    m_files->setIconProvider(new QFileIconProvider);
    m_cmdHistory = new QStringListModel(this);

    setUpdatesEnabled(false);
    show();
    message("dummy"); // QTextBrowser needs a kick in the butt, cause the first document size isn't properly calculated (at all)
    setCurrentWidget(m_status);
    adjustGeometry();
    activateWindow();
    setUpdatesEnabled(true);
    connect(m_input, &QLineEdit::textChanged, [=](const QString &text) {
        if (text.isEmpty()) {
            m_input->hide();
            if (currentWidget() == m_list && (m_list->model() == m_external || m_list->model() == m_notifications->model()))
                return;
            if (currentWidget() != m_disp)
                setCurrentWidget(m_status);
            return;
        }
        QFont fnt = font();
        fnt.setPointSize((2.0f-qMin(1.2f, text.size()/80.0f))*fnt.pointSize());
        m_input->setFont(fnt);
        if (currentWidget() == m_status && text.size() == 1) {
            m_list->setModel(m_applications);
            filterInput();
            setCurrentWidget(m_list);
        }

        const QSize ts = m_input->fontMetrics().boundingRect("xx" + text).size();
        const int w = m_input->style()->sizeFromContents(QStyle::CT_LineEdit, nullptr, ts, m_input).width();
        m_input->setGeometry((width() - w)/2, (height() - 2*ts.height())/2, w, 2*ts.height());
        m_input->show();
        m_input->setFocus();
    });
    m_list->setFocusProxy(m_input);
    m_list->viewport()->setFocusProxy(m_input);
    m_disp->setFocusProxy(m_input);
    m_status->setFocusProxy(m_input);
    setFocusProxy(m_input);
    m_autoHide.setInterval(3000);
    m_autoHide.setSingleShot(true);
    connect(&m_autoHide, &QTimer::timeout, this, &QWidget::hide);
}

void Qiq::reconfigure() {
    QSettings settings("qiq");

    QFile sheet(QStandardPaths::locate(QStandardPaths::AppDataLocation, settings.value("Style", "default.css").toString()));
    if (sheet.exists() && sheet.open(QIODevice::ReadOnly | QIODevice::Text))
        qApp->setStyleSheet(QString::fromLocal8Bit(sheet.readAll()));
    sheet.close();

    m_aha = settings.value("AHA").toString();
    m_qalc = settings.value("CALC").toString();
    m_term = settings.value("TERMINAL", qEnvironmentVariable("TERMINAL")).toString();
    m_cmdCompletion = settings.value("CmdCompleter").toString();
    m_cmdCompletionSep = settings.value("CmdCompletionSep").toString();
    m_historyPath = settings.value("HistoryPath").toString();
    if (!m_historyPath.isEmpty() && !m_historySaver) {
        m_historySaver = new QTimer(this);
        m_historySaver->setSingleShot(true);
        m_historySaver->setInterval(300000); // 5 minutes
        connect(m_historySaver, &QTimer::timeout, this, &Qiq::writeHistory);
    }

    QFont gaugeFont = QFont(settings.value("GaugeFont").toString());
    QStringList gauges = settings.value("Gauges").toStringList();
    const QSize oldDefaultSize = m_defaultSize;
    m_defaultSize = QSize(settings.value("Width", 640).toUInt(), settings.value("Height", 320).toUInt());
    if (oldDefaultSize != m_defaultSize)
        resize(m_defaultSize);
    int iconSize = settings.value("IconSize", 48).toUInt();
    m_list->setIconSize(QSize(iconSize,iconSize));
    QList<Gauge*> oldGauges = m_status->findChildren<Gauge*>();
    for (const QString &gauge : gauges) {
        settings.beginGroup(gauge);
        Gauge *g = m_status->findChild<Gauge*>(gauge);
        if (g) {
            oldGauges.removeAll(g);
        } else {
            g = new Gauge(m_status);
            g->setObjectName(gauge);
        }

        g->setFont(gaugeFont);
        for (int i = 0; i < 3; ++i) {
            g->setSource(settings.value(QString("Source%1").arg(i+1)).toString(), i);
            g->setRange(settings.value(QString("Min%1").arg(i+1), 0).toInt(),
                        settings.value(QString("Max%1").arg(i+1), 100).toInt(), i);
            g->setColors(settings.value(QString("ColorLow%1").arg(i+1)).value<QColor>(),
                         settings.value(QString("ColorHigh%1").arg(i+1)).value<QColor>(), i);
        }
        g->setLabel(settings.value("Label").toString());
        g->setInterval(settings.value("Interval", 1000).toUInt());
        g->setToolTip(settings.value("Tooltip").toString(),
                      settings.value("TooltipCacheTimeout", 1000).toUInt());
        g->setMouseAction(settings.value("ActionLMB").toString(), Qt::LeftButton);
        g->setMouseAction(settings.value("ActionRMB").toString(), Qt::RightButton);
        g->setMouseAction(settings.value("ActionMMB").toString(), Qt::MiddleButton);
        g->setWheelAction(settings.value("ActionWUp").toString(), Qt::UpArrow);
        g->setWheelAction(settings.value("ActionWDown").toString(), Qt::DownArrow);
        QString s = settings.value("Align", "Center").toString();
        Qt::Alignment a;
        if (s.contains("top", Qt::CaseInsensitive))
            a |= Qt::AlignTop;
        else if (s.contains("bottom", Qt::CaseInsensitive))
            a |= Qt::AlignBottom;
        if (s.contains("left", Qt::CaseInsensitive))
            a |= Qt::AlignLeft;
        else if (s.contains("right", Qt::CaseInsensitive))
            a |= Qt::AlignRight;
        if (s.contains("center", Qt::CaseInsensitive)) {
            if (!(a & (Qt::AlignTop|Qt::AlignBottom)))
                a |= Qt::AlignVCenter;
            if (!(a & (Qt::AlignLeft|Qt::AlignRight)))
                a |= Qt::AlignHCenter;
        }
        g->setPosition(a, settings.value("OffsetX", 0).toInt(), settings.value("OffsetY", 0).toInt());
        g->setSize(settings.value("Size", 128).toInt());
        settings.endGroup();
    }

    for (Gauge *og : oldGauges)
        og->deleteLater();

    m_aliases.clear();
    settings.beginGroup("Aliases");
    for (const QString &key : settings.childKeys())
        m_aliases.insert(key, settings.value(key).toString());
    settings.endGroup();
    if (oldDefaultSize != m_defaultSize)
        QMetaObject::invokeMethod(this, &Qiq::adjustGeometry, Qt::QueuedConnection);
}

void Qiq::makeApplicationModel() {
    const QString de_DE = QLocale::system().name();
    const QString de = de_DE.split('_').first();
    const QString name_de_DE = "Name[" + de_DE + "]";
    const QString name_de = "Name[" + de + "]";
    const QString comment_de_DE = "Comment[" + de_DE + "]";
    const QString comment_de = "Comment[" + de + "]";
    const QString keywords_de_DE = "Keywords[" + de_DE + "]";
    const QString keywords_de = "Keywords[" + de + "]";
    m_applications = new QStandardItemModel;
    const QStringList paths = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    QSet<QString> augmented;
    for (const QString &path : paths) {
        QDir dir(path);
        if (!dir.exists())
            continue;
        const QStringList files = dir.entryList(QStringList() << "*.desktop", QDir::Files|QDir::Readable);
        for (const QString &file : files) {
            if (augmented.contains(file))
                continue;
            augmented.insert(file);
            QSettings service(path + QDir::separator() + file, QSettings::IniFormat);
            service.beginGroup("Desktop Entry");
            if (service.value("Type").toString() != "Application")
                continue;
#define LOCAL_AWARE(_V_, _D_) QString _V_ = service.value(_V_##_de_DE).toString(); \
                              if (_V_.isEmpty()) _V_ = service.value(_V_##_de).toString(); \
                              if (_V_.isEmpty()) _V_ = service.value(_D_).toString();
            LOCAL_AWARE(name, "Name")
            if (name.isEmpty())
                continue;
            // only type and name are mandatory, but if there's no executable, this isn't any useful
            const QString exec = service.value("Exec").toString();
            if (exec.isEmpty())
                continue;
            QStandardItem *item = new QStandardItem(QIcon::fromTheme(service.value("Icon").toString()), name);
            item->setData(exec, AppExec);
            LOCAL_AWARE(comment, "Comment")
            item->setData(comment, AppComment);
            item->setData(service.value("Path"), AppPath);
            item->setData(service.value("Terminal", false).toBool(), AppNeedsTE);
            item->setData(service.value("Categories").toString().split(';'), AppCategories);
            LOCAL_AWARE(keywords, "Keywords")
            item->setData(keywords.split(';'), AppKeywords);
            // mimetype
            m_applications->appendRow(item);
        }
    }
}

void Qiq::adjustGeometry() {
    if (currentWidget() != m_disp) {
        m_disp->setMinimumSize(QSize(0,0));
        setMinimumSize(QSize(0,0));
    }
    if (currentWidget() == m_disp) {
        adjustSize();
    } else if (currentWidget() == m_status) {
        resize(m_defaultSize);
    } else {
        QSize sz = m_defaultSize;
        if (currentWidget() == m_list) {
            QRect r = m_list->visualRect(m_list->model()->index(m_lastVisibleRow, 0, m_list->rootIndex()));
            sz.setHeight(qMax(qMin(r.bottom()+r.height(), sz.height()), 3*m_input->height()));
        }
        resize(sz);
    }
    QRect r = m_input->rect();
    r.moveCenter(rect().center());
    m_input->setGeometry(r);
    if (const QScreen *screen = windowHandle()->screen()) {
        QRect r = rect();
        r.moveCenter(screen->geometry().center());
        setGeometry(r);
    } else {
        qDebug() << "fuck wayland";
    }
}

void Qiq::enterEvent(QEnterEvent *ee) {
    m_autoHide.stop(); // user interaction
    activateWindow();
    QStackedWidget::enterEvent(ee);
}

bool Qiq::eventFilter(QObject *o, QEvent *e) {
    if (o == m_input && e->type() == QEvent::KeyPress) {
        m_autoHide.stop(); // user interaction
        const int key = static_cast<QKeyEvent*>(e)->key();
        if (key == Qt::Key_Tab) {
            if (m_input->text().isEmpty()) {
                if (currentWidget() == m_status) {
                    m_list->setModel(m_applications);
                    filter(QString(), Partial);
                    setCurrentWidget(m_list);
                } else if (currentWidget() == m_list) {
                    if (m_list->model() == m_applications) {
                        m_list->setModel(m_bins);
                        filter(QString(), Begin);
                    } else if (m_list->model() == m_bins) {
                        m_list->setModel(m_external);
                        filter(QString(), Partial);
                    } else if (m_list->model() == m_external) {
                        m_list->setModel(m_applications);
                        setCurrentWidget(m_disp);
                    }
                } else if (currentWidget() == m_disp) {
                    setCurrentWidget(m_status);
                }
            } else {
                explicitlyComplete();
            }
            return true;
        }
        if ((key == Qt::Key_PageUp || key == Qt::Key_PageDown) && currentWidget() == m_list) {
            m_list->setEnabled(true);
            QApplication::sendEvent(currentWidget(), e);
            insertToken();
            return true;
        }
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            if (currentWidget() == m_list) {
                m_list->setEnabled(true);
                QApplication::sendEvent(currentWidget(), e);
                insertToken();
            } else {
                int idx = m_currentHistoryIndex;
                if (idx < 0) {
                    m_inputBuffer = m_input->text();
                    idx = m_history.size();
                }
                if (key == Qt::Key_Up)
                    --idx;
                else
                    ++idx;
                if (idx >= m_history.size()) {
                    m_currentHistoryIndex = -1;
                    m_input->setText(m_inputBuffer);
                } else if (idx > -1) {
                    m_currentHistoryIndex = idx;
                    m_input->setText(m_history.at(idx));
                }
            }
            return true;
        }
        if (key == Qt::Key_Escape) {
            if (m_input->echoMode() == QLineEdit::Password) {
                m_input->clear();
                m_input->hide(); // force
                m_input->setEchoMode(QLineEdit::Normal);
            } else if (currentWidget() == m_list && m_list->model() == m_cmdHistory) {
                m_list->setCurrentIndex(QModelIndex());
                runInput();
                m_input->setText(m_inputBuffer);
            } else if (m_input->isVisible()) {
                m_input->clear();
                m_input->hide(); // force
            } else if (currentWidget() == m_disp) {
                setCurrentWidget(m_status);
            } else if (currentWidget() == m_list && m_list->model() == m_external) {
                m_externalReply = QString(""); // empt, not null!
                if (!m_wasVisble)
                    hide();
                setCurrentWidget(m_status);
            } else if (currentWidget() == m_list && m_list->model() == m_notifications->model()) {
                setCurrentWidget(m_status);
            } else { //QApplication::quit();
                m_externalReply = QString(""); // empt, not null!
                hide();
            }
            return true;
        }
        if (key == Qt::Key_Space || (m_list->model() == m_files && static_cast<QKeyEvent*>(e)->text() == "/")) {
            if (m_input->selectionEnd() == m_input->text().size()) {
                m_input->deselect();
                m_input->setCursorPosition(m_input->text().size());
            }
            return false;
        }
        if (key == Qt::Key_Enter || key == Qt::Key_Return) {
            if (runInput()) {
                m_input->clear();
                m_input->hide(); // force
            }
            return true;
        }
        if (key == Qt::Key_R && (static_cast<QKeyEvent*>(e)->modifiers() & Qt::ControlModifier)) {
            m_inputBuffer = m_input->text();
            m_cmdHistory->setStringList(m_history);
            m_list->setModel(m_cmdHistory);
            filter(m_inputBuffer, Partial);
            setCurrentWidget(m_list);
            return true;
        }
        if (key == Qt::Key_N && (static_cast<QKeyEvent*>(e)->modifiers() & (Qt::ControlModifier|Qt::ShiftModifier))) {
            m_input->clear();
            m_list->setModel(m_notifications->model());
            filter(QString(), Partial);
            setCurrentWidget(m_list);
            return true;
        }
        if (key == Qt::Key_Delete && currentWidget() == m_list &&
            // only if the user doesn't want to delete texr
            !m_input->selectionLength() && m_input->cursorPosition() == m_input->text().size() &&
            // … and valid indices
            m_list->currentIndex().isValid()) {
            if (m_list->model() == m_cmdHistory) {
                m_history.removeAll(m_list->currentIndex().data().toString());
                m_cmdHistory->setStringList(m_history);
            } else if (m_list->model() == m_notifications->model()) {
                m_notifications->purge(m_list->currentIndex().data(Notifications::ID).toUInt());
            }
        }
        if (!m_input->isVisible() && static_cast<QKeyEvent*>(e)->text().isEmpty()) {
            QApplication::sendEvent(currentWidget(), e);
            return true;
        }
        if (currentWidget() == m_disp) {
            if (key == Qt::Key_PageUp && !m_input->text().isEmpty()) {
                m_disp->find(m_input->text(), QTextDocument::FindBackward);
                return true;
            }
            if (key == Qt::Key_PageDown && !m_input->text().isEmpty()) {
                m_disp->find(m_input->text());
                return true;
            }
            if (!static_cast<QKeyEvent*>(e)->text().isEmpty()) {
                QTimer::singleShot(0, [=](){ // text needs to be updated first
                    if (!m_disp->find(m_input->text()))
                        m_disp->find(m_input->text(), QTextDocument::FindBackward);
                    });
                // fall through, input still needs to be handled
            }
        }
        return false;
    }
    return false;
}

static QString previousNeedle;
static bool cycleResults = false;
void Qiq::explicitlyComplete() {
    const QString lastToken = m_input->text().left(m_input->cursorPosition()).section(whitespace, -1, -1);
    if (currentWidget() != m_list)
        cycleResults = false;
    if (cycleResults) {
        QModelIndex oldIndex = m_list->currentIndex();
        QKeyEvent ke(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        m_list->setEnabled(true);
        QApplication::sendEvent(m_list, &ke);
        if (oldIndex == m_list->currentIndex()) {
            QKeyEvent ke(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
            QApplication::sendEvent(m_list, &ke);
            if (m_list->isRowHidden(0)) {
                QKeyEvent ke(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
                QApplication::sendEvent(m_list, &ke);
            }
        }
        insertToken();
        return;
    }
    QString path = lastToken;
    if (path.startsWith('~'))
        path.replace(0,1,QDir::homePath());
    QFileInfo fileInfo(path);
    QDir dir = fileInfo.dir();
    if (dir.exists() && (dir != QDir::current() || lastToken.contains('/'))) {
        setCurrentWidget(m_list);
        m_list->setModel(m_files);
        cycleResults = true;
        if (m_files->rootPath() == dir.absolutePath()) {
            insertToken();
            return; // idempotent, leave the directory as is
        }
        m_files->setRootPath(dir.absolutePath());
        m_list->setCurrentIndex(QModelIndex());
        QModelIndex newRoot = m_files->index(m_files->rootPath());
        m_list->setRootIndex(newRoot);
        previousNeedle.clear();
        // this can take a moment to feed the model
        connect(m_files, &QFileSystemModel::directoryLoaded, this, [=](const QString &path) {
                        if (path == m_files->rootPath()) {
                            m_files->sort(0);
                            filter(fileInfo.fileName(), Begin);
                        }
                        cycleResults = true;
                        }, Qt::SingleShotConnection);
        return;
    }
    auto stripInstruction = [=](QString &token) {
        if (token.startsWith('=') || token.startsWith('?') || token.startsWith('!') || token.startsWith('#'))
            token.remove(0,1);
    };
    QString lastCmd = m_input->text().left(m_input->cursorPosition()).section('|', -1, -1);
    static const QRegularExpression leadingWS("^\\s*");
    lastCmd.remove(leadingWS);
    stripInstruction(lastCmd);
    if (m_bins->stringList().contains(lastCmd.section(whitespace, 0, 0).trimmed())) { // first token is a known binary
        if (!m_cmdCompletion.isEmpty()) {
            QProcess complete;
            complete.start(m_cmdCompletion, QStringList() << lastCmd);
            if (complete.waitForFinished(2000)) {
                if (!m_cmdCompleted)
                    m_cmdCompleted = new QStringListModel(this);
                QStringList completions = QString::fromLocal8Bit(complete.readAllStandardOutput()).split('\n');
                if (completions.last().isEmpty())
                    completions.removeLast();
                completions.removeDuplicates();
                m_cmdCompleted->setStringList(completions);
                m_list->setModel(m_cmdCompleted);
                setCurrentWidget(m_list);
                filterInput();
            }
        }
    } else {
        m_list->setModel(m_bins);
        previousNeedle.clear();
        lastCmd = lastToken;
        stripInstruction(lastCmd);
        filter(lastCmd, Begin);
        setCurrentWidget(m_list);
    }
    cycleResults = true;
}

void Qiq::tokenUnderCursor(int &left, int &right) {
    const QString text = m_input->text();
    left = text.lastIndexOf(whitespace, m_input->cursorPosition() - 1) + 1;
    right = text.indexOf(whitespace, m_input->cursorPosition());
    if (right < 0)
        right = text.length();
}

void Qiq::filter(const QString needle, MatchType matchType) {
    if (!needle.isNull()) // artificial to prime geometry adjustment
        cycleResults = false;
    if (!m_list->model())
        return;
    bool shrink = false;
    const int rows = m_list->model()->rowCount(m_list->rootIndex());
    int visible = 0;
    static int prevVisible = 0;
    int firstVisRow = m_lastVisibleRow = -1;

    if (m_list->model() == m_applications) {
        QStringList tokens = needle.split(whitespace);
        for (int i = 0; i < rows; ++i) {
            const QModelIndex idx = m_list->model()->index(i, 0, m_list->rootIndex());
            bool vis = false;
            for (const QString &token : tokens) {
                if ((vis = idx.data().toString().contains(token, Qt::CaseInsensitive))) continue;
                if ((vis = idx.data(AppExec).toString().contains(token, Qt::CaseInsensitive))) continue;
                if ((vis = idx.data(AppComment).toString().contains(token, Qt::CaseInsensitive))) continue;
                const QStringList cats = idx.data(AppCategories).toStringList();
                for (const QString &cat : cats) {
                    if ((vis = cat.contains(token, Qt::CaseInsensitive))) break;
                }
                const QStringList keys = idx.data(AppKeywords).toStringList();
                for (const QString &key : keys) {
                    if ((vis = key.contains(token, Qt::CaseInsensitive))) break;
                }
                if (!vis) break;
            }
            if (vis) {
                m_lastVisibleRow = i;
                if (firstVisRow < 0)
                    firstVisRow = i;
            }
            m_list->setRowHidden(i, !(vis && ++visible));
        }
    } else if (m_list->model() == m_notifications->model()) {
        QStringList tokens = needle.split(whitespace);
        for (int i = 0; i < rows; ++i) {
            const QModelIndex idx = m_list->model()->index(i, 0, m_list->rootIndex());
            bool vis = false;
            for (const QString &token : tokens) {
                if ((vis = idx.data().toString().contains(token, Qt::CaseInsensitive))) continue;
                if ((vis = idx.data(Qt::ToolTipRole).toString().contains(token, Qt::CaseInsensitive))) continue;
                if ((vis = idx.data(Notifications::AppName).toString().contains(token, Qt::CaseInsensitive))) continue;
                if (!vis) break;
            }
            if (vis) {
                m_lastVisibleRow = i;
                if (firstVisRow < 0)
                    firstVisRow = i;
            }
            m_list->setRowHidden(i, !(vis && ++visible));
        }
    } else if (matchType == Begin) {
        for (int i = 0; i < rows; ++i) {
            const bool vis = m_list->model()->index(i, 0, m_list->rootIndex()).data().toString().startsWith(needle, Qt::CaseInsensitive);
            if (vis) {
                m_lastVisibleRow = i;
                if (firstVisRow < 0)
                    firstVisRow = i;
            }
            m_list->setRowHidden(i, !(vis && ++visible));
        }
        shrink = previousNeedle.startsWith(needle, Qt::CaseInsensitive);
    } else { // if (matchType == Partial)
        QStringList sl = needle.split(whitespace, Qt::SkipEmptyParts);
        for (int i = 0; i < rows; ++i) {
            const QString &hay = m_list->model()->index(i, 0, m_list->rootIndex()).data().toString();
            bool vis = true;
            for (const QString &s : sl) {
                if (!hay.contains(s, Qt::CaseInsensitive)) {
                    vis = false;
                    break;
                }
            }
            if (vis) {
                m_lastVisibleRow = i;
                if (firstVisRow < 0)
                    firstVisRow = i;
            }
            m_list->setRowHidden(i, !(vis && ++visible));
        }
        shrink = previousNeedle.contains(needle, Qt::CaseInsensitive);
    }
    previousNeedle = needle;
    const int row = m_list->currentIndex().row();
    bool looksLikeCommand = false;
    if (m_list->currentIndex().isValid() && (m_list->model() == m_applications || m_list->model() == m_external))
        looksLikeCommand = m_input->text().contains('|') || (m_input->text().trimmed().contains(whitespace) && m_bins->stringList().contains(m_input->text().split(whitespace).first()));
    if (looksLikeCommand) {
        // if the user seems to enter a command, unselect any entries and force reselection
        m_list->setCurrentIndex(QModelIndex());
    } else if (visible > 0 && (row < 0 || m_list->isRowHidden(row))) {
        m_list->setCurrentIndex(m_list->model()->index(firstVisRow, 0, m_list->rootIndex()));
    } else if (!visible || (visible > 1 && shrink && prevVisible == 1)) {
        m_list->setCurrentIndex(QModelIndex());
    }
    m_list->setEnabled(m_list->currentIndex().isValid());
    prevVisible = visible;
    if (visible == 1 && !shrink && !needle.isEmpty()) {
        QTimer::singleShot(1, this, &Qiq::insertToken); // needs to be delayed to trigger the textChanged after the actual edit
    }
    adjustGeometry();
}

void Qiq::filterInput() {
    if (m_list->model() == m_applications || m_list->model() == m_external || m_list->model() == m_cmdHistory)
        return filter(m_input->text(), Partial);

    QString text = m_input->text();
    int left, right;
    tokenUnderCursor(left, right);
    text = text.mid(left, right - left);
    if (m_list->model() == m_files) {
        QFileInfo fileInfo(text);
        const QString path = fileInfo.dir().absolutePath();
        if (path != m_files->rootPath()) {
            m_files->setRootPath(path);
            m_list->setCurrentIndex(QModelIndex());
            QModelIndex newRoot = m_files->index(m_files->rootPath());
            m_list->setRootIndex(newRoot);
            m_files->fetchMore(newRoot);
        }
        text = fileInfo.fileName();
    } else if (m_list->model() == m_bins && text.isEmpty()) {
        setCurrentWidget(m_status);
    }
    filter(text, Begin);
}

void Qiq::insertToken() {
    if (m_list->model() == m_applications)
        return; // nope. Never.
    if (m_list->model() == m_external) {
        if (m_externCmd == "_qiq") { // this is because if the user wants the output as a list they might want to do some with those values
            m_input->setText(m_list->currentIndex().data().toString());
        }
        return;
    }
    QString newToken = m_list->currentIndex().data().toString();
    if (m_list->model() == m_files) {
        QString path = m_files->rootPath();
        if (!path.endsWith("/"))
            path += "/";
        newToken = path + newToken;
        if (newToken.contains(whitespace))
            newToken = "\"" + newToken + "\"";
    } else if (m_list->model() == m_cmdHistory) {
        m_input->setText(newToken);
        return;
    } else if (m_list->model() == m_cmdCompleted) {
        if (!m_cmdCompletionSep.isEmpty()) {
            newToken = newToken.section(m_cmdCompletionSep, 0, 0);
            newToken.remove('\r'); // zsh completions at times at least have that, probably to control the cursor
            newToken.remove('\t'); newToken.remove('\a'); // just for good measure
        }
    }
    QString text = m_input->text();

    int pos = m_input->selectionStart();
    if (pos > -1) {
        int len = m_input->selectionLength();
        text.replace(pos, len, newToken);
        m_input->setText(text);
    } else {
        int left, right;
        tokenUnderCursor(left, right);
        const QChar firstChar = text.at(left);
        if (firstChar == '=' || firstChar == '?' || firstChar == '!' || firstChar == '#')
            ++left;
        text.replace(left, right - left, newToken);
        pos = -(left+newToken.size());
    }
    m_input->setText(text);
    if (pos > -1)
        m_input->setSelection(pos, newToken.length());
    else
        m_input->setCursorPosition(-pos);
}

bool mightBeRichText(const QString &text) {
    QString sample = text; //.left(512);
    if (sample.contains("<html>", Qt::CaseInsensitive))
        return true;
    if (sample.contains("<!DOCTYPE HTML<html>", Qt::CaseInsensitive))
        return true;
    if (sample.contains("<!DOCTYPE HTML<html>", Qt::CaseInsensitive))
        return true;
    if (sample.contains("<!--"))
        return true;
    return Qt::mightBeRichText(sample.remove('\n'));
}

void Qiq::message(const QString &string) {
    m_autoHide.stop(); // user needs to read this ;)
    m_disp->setMinimumWidth(1);
    m_disp->setMinimumHeight(1);
    m_disp->resize(0,0);
    m_disp->setHtml(string);
    QSize max(800,800);
    if (const QScreen *screen = windowHandle()->screen()) {
        max = screen->geometry().size()*0.666666667;
    }
    m_disp->setMinimumWidth(qMax(m_defaultSize.width(), qMin(max.width(), 12+qCeil(m_disp->document()->size().width()))));
    m_disp->setMinimumHeight(qMin(max.height(), 12+qCeil(m_disp->document()->size().height())));
    if (currentWidget() != m_disp)
        setCurrentWidget(m_disp);
    else
        adjustGeometry();
}

void Qiq::printOutput(int exitCode) {
    QProcess *process = qobject_cast<QProcess*>(sender());
    if (!process) {
        qDebug() << "wtf got us here?" << sender();
        return;
    }
    QString output;
    if (exitCode) {
        output = "<h3 align=center style=\"color:#d01717;\">" + process->program() + " " + process->arguments().join(" ") + "</h3><p style=\"color:#d01717;\">";
        QByteArray error = process->readAllStandardError();
        if (!error.isEmpty()) {
            output += QString::fromLocal8Bit(error).toHtmlEscaped();
        output += "</p>";
        }
    } else {
        m_disp->setTextColor(m_disp->palette().color(m_disp->foregroundRole()));
    }
    bool showAsList = false;
    QByteArray stdout = process->readAllStandardOutput();
    if (!stdout.isEmpty()) {
        if (process->property("qiq_type").toString() == "math") {
            output += "<pre align=center style=\"font-size:xx-large;\"><br><br>" + QString::fromLocal8Bit(stdout) + "</pre>";
        } else if (process->property("qiq_type").toString() == "list") {
            showAsList = true;
            output = QString::fromLocal8Bit(stdout);
        } else if (mightBeRichText(QString::fromLocal8Bit(stdout.left(512)))) {
            output += QString::fromLocal8Bit(stdout);
        } else {
            if (m_aha.isNull()) {
                if (m_bins->stringList().contains("ansifilter"))
                    m_aha = "ansifilter -f -H";
                else if (m_bins->stringList().contains("aha"))
                    m_aha = "aha -x -n";
                else
                    m_aha = "";
            }
            if (!m_aha.isEmpty() && stdout.contains("\e[")) {
                QProcess aha;
                aha.startCommand(m_aha);
                if (aha.waitForStarted(250)) {
                    aha.write(stdout);
                    aha.closeWriteChannel();
                    if (aha.waitForFinished(250))
                        stdout = aha.readAllStandardOutput();
                }
                output += "<pre>" + QString::fromLocal8Bit(stdout) + "</pre>";
            } else {
                output += "<pre>" + QString::fromLocal8Bit(stdout).toHtmlEscaped() + "</pre>";
            }
        }
    }

    if (!output.isEmpty()) {
        m_autoHide.stop(); // user may wanna read this ;)
        if (showAsList) {
            m_externCmd = "_qiq";
            if (!m_external)
                m_external = new QStandardItemModel(this);
            m_external->clear();
            const QStringList lines = output.split('\n');
            for (const QString &l : lines)
                m_external->appendRow(new QStandardItem(l));
            m_list->setModel(m_external);
            filter(QString(), Partial);
            if (currentWidget() != m_list)
                setCurrentWidget(m_list);
            else
                adjustGeometry();
        } else {
            message(output);
        }
    }
}


bool Qiq::runInput() {
    if (m_input->echoMode() == QLineEdit::Password) {
        // got password password, fix mode and return
        m_input->setEchoMode(QLineEdit::Normal);
        return false;
    }

    QAbstractItemModel *currentModel = nullptr;
    if (currentWidget() == m_list)
        currentModel = m_list->model();

    // filter from custom list ==========================================================================================================
    if (currentModel == m_external && m_externCmd != "_qiq") {
        QModelIndex entry = m_list->currentIndex();
        if (entry.isValid()) {
            bool ret = false;
            QString v = entry.data(AppExec).toString();
            if (v.isEmpty())
                v = entry.data().toString();
            auto sed = [&](const QString cmd) {
                const int s = cmd.size();
                QString sedSep = m_externCmd.mid(s,1);
                if (!sedSep.isEmpty()) {
                    QStringList parts = m_externCmd.mid(s+1).split(sedSep);
                    for (int i = 0; i < parts.size(); i+=2) {
                        QRegularExpression reg(parts.at(i));
                        if (parts.size() > i+1)
                            v.replace(reg, parts.at(i+1));
                        else
                            v.remove(reg);
                    }
                }
            };
            if (m_externCmd.startsWith("%clip")) {
                sed("%clip");
                QGuiApplication::clipboard()->setText(v, QClipboard::Clipboard);
                QGuiApplication::clipboard()->setText(v, QClipboard::Selection);
                ret = true;
            } else if (m_externCmd.startsWith("%print")) {
                sed("%print");
                m_externalReply = v;
                ret = true;
            } else {
                ret = QProcess::startDetached(m_externCmd, QStringList() << v);
            }
            if (!m_wasVisble)
                hide();
            else
                m_autoHide.start(3000);
            return ret;
        }
//        return false; // No! fall through for regular command handling
    }
    // =============================================================================================================================

    // filter from history ==========================================================================================================
    if (currentModel == m_cmdHistory) {
        QModelIndex entry = m_list->currentIndex();
        if (entry.isValid())
            m_input->setText(entry.data().toString());
        m_list->setModel(m_bins);
        setCurrentWidget(m_status);
        m_cmdHistory->setStringList(QStringList());
        m_autoHide.stop(); // SIC! Just in case it's running
        return false; // SIC! we don't want the input to be accepted, just changed
    }
    // =============================================================================================================================

    // recall notification =========================================================================================================
    if (currentModel == m_notifications->model()) {
        QModelIndex entry = m_list->currentIndex();
        if (entry.isValid())
            m_notifications->recall(entry.data(Notifications::ID).toUInt());
        m_autoHide.stop(); // SIC! Just in case it's running
        return false; // SIC! the user might want to see more notifications
    }
    // =============================================================================================================================

    QString command = m_input->text();
    static QRegularExpression hometilde("(^|\\W)~(/|\\W|$)");
    command.replace(hometilde, "\\1" + QDir::homePath() + "\\2");

    if (command.isEmpty()) {
        QModelIndex entry = m_list->currentIndex();
        if (entry.isValid())
            command = entry.data().toString();
    }

    // open file ==================================================================================================================
    if (QFileInfo::exists(command)) {
        m_autoHide.start(1000);
        return QProcess::startDetached("xdg-open", QStringList() << command);
    }
    if (command.startsWith('"') && command.endsWith('"') && QFileInfo::exists(command.sliced(1,command.size()-2))) {
        m_autoHide.start(1000);
        return QProcess::startDetached("xdg-open", QStringList() << command.sliced(1,command.size()-2));
    }
    // ============================================================================================================================

    // application list ===========================================================================================================
    if (currentModel == m_applications) {
        QModelIndex entry = m_list->currentIndex();
        if (entry.isValid()) {
            QString exec = entry.data(AppExec).toString();
            exec.remove(QRegularExpression("%[fFuU]"));
            m_autoHide.start(500);
            QStringList args;
            if (entry.data(AppNeedsTE).toBool()) {
                if (m_term.isNull()) {
                    message(tr("<h1 align=center>TERMINAL required</h1><i>%1</i> needs a terminal\nPlease configure the \"TERMINAL\" setting or environment variable.").arg(entry.data().toString()));
                    return false;
                }
                args = QProcess::splitCommand(m_term) + QProcess::splitCommand(exec);
            } else {
                args = QProcess::splitCommand(exec);
            }
            if (!args.isEmpty())
                exec = args.takeFirst();
            return QProcess::startDetached(exec, args, entry.data(AppPath).toString());
        }
    }

    // custom command ===========================================================================================================
    QProcess *process = new QProcess(this);
    enum Type { Normal = 0, NoOut, ForceOut, Math, List };
    Type type = Normal;
    if (command.startsWith("=")) {
        type = Math;
        command.remove(0,1);
    } else if (command.startsWith("?")) {
        type = ForceOut;
        command.remove(0,1);
    } else if (command.startsWith("!")) {
        type = NoOut;
        command.remove(0,1);
    } else if (command.startsWith("#")) {
        type = List;
        command.remove(0,1);
        process->setProperty("qiq_type", "list");
    }
    command = command.trimmed();
    if (command.contains('|')) {
        QStringList components = command.split('|');
        command = components.takeLast().trimmed();
        QProcess *sink = process;
        for (int i = components.size() - 1; i >= 0; --i) {
            const QString component = components.at(i).trimmed();
            QProcess *feeder = new QProcess(this);
            connect(feeder, &QProcess::finished, feeder, &QObject::deleteLater);
            feeder->setStandardOutputProcess(sink);
            sink = feeder;
            feeder->startCommand(component);
        }
    }

    QTimer *detachIO = nullptr;
    QMetaObject::Connection processDoneHandler;
    if (type != NoOut)
        processDoneHandler = connect(process, &QProcess::finished, this, &Qiq::printOutput);
    if (type == Normal) { // NoOut is always detached and we want the output of everyhing else, no matter how long it takes and it doesn't need to survive us
        process->setChildProcessModifier([] {::setsid(); });
        detachIO = new QTimer(this);
        detachIO->setSingleShot(true);
        connect (detachIO, &QTimer::timeout, this, [=](){
            if (processDoneHandler) {
                process->closeReadChannel(QProcess::StandardOutput);
                process->closeReadChannel(QProcess::StandardError);
                process->closeWriteChannel();
                disconnect(processDoneHandler);
            }
            detachIO->deleteLater();
        });
        detachIO->start(3000);
    }
    connect(process, &QProcess::finished, process, &QObject::deleteLater);

    bool ret = false;
    if (type != Math) {
        int sp = command.indexOf(whitespace);
        if (sp < 0)
            sp = command.size();
        const QString bin = command.left(sp);
        command.replace(0, sp, m_aliases.value(bin, bin));
        // the alias could have introduced an instruction
        // strip that and adhere, but don't override explict Types
        if (command.startsWith("?")) {
            if (type == Normal) type = ForceOut;
            command.remove(0,1);
        } else if (command.startsWith("!")) {
            if (type == Normal) type = NoOut;
            command.remove(0,1);
        } else if (command.startsWith("#")) {
            if (type == Normal) {
                type = List;
                process->setProperty("qiq_type", "list");
            }
            command.remove(0,1);
        }
        QStringList tokens = command.split(whitespace);
        for (const QString &token : tokens) {
            if (token.startsWith('$')) {
                QString env = qEnvironmentVariable(token.mid(1).toUtf8().data());
                if (!env.isNull())
                    command.replace(token, env);
            }
        }

        QString exec = command;
        QStringList args = QProcess::splitCommand(exec);
        if (!args.isEmpty())
            exec = args.takeFirst();
        if (type == NoOut) {
            ret = QProcess::startDetached(exec, args);
        } else {
            if (type == ForceOut)
                message("<h3 align=center>" + tr("Waiting for output…") + "</h3>");
            const bool isSudo((exec == "sudo" || exec == "sudoedit") && !args.contains("-k")); // "sudo -k" fails w/ -n and never needs credentials
            if (isSudo) {
                args.prepend("-n");
                disconnect(process, &QProcess::finished, process, &QObject::deleteLater);
                process->setChildProcessModifier([] {}); // ::setsid() would lose the cached credentials
            }
            process->start(exec, args);
            ret = process->waitForStarted(250);
            if (ret && isSudo) {
                process->waitForFinished(250);
                if (process->state() == QProcess::NotRunning && process->exitCode()) {
                    detachIO->stop();
                    message("<h3 align=center>" + command + "</h3><h1 align=center>" + tr("…enter your sudo password…") + "</h1><p align=center>" + tr("(press escape to abort)") + "</p>");
                    m_input->clear();
                    m_input->setEchoMode(QLineEdit::Password);
                    QElapsedTimer time;
                    while (m_input->echoMode() == QLineEdit::Password) {
                        time.start();
                        QApplication::processEvents();
                        QThread::msleep(33-time.elapsed()); // maintain 30fps but don't live-lock in processEvents()
                    }
                    if (m_input->text().isEmpty()) {
                        message("<h3 align=center>" + command + "</h3><h1 align=center>" + tr("aborted") + "</h1>");
                        setCurrentWidget(m_status);
                        if (detachIO) detachIO->deleteLater();
                        process->deleteLater();
                        return false;
                    } else {
                        message("<h3 align=center>" + command + "</h3><h1 align=center>" + tr("Password entered") + "</h1>");
                        args.replace(0, "-S"); //  -n has run it's course and would spoil -S
                        connect(process, &QProcess::finished, process, &QObject::deleteLater);
                        if (detachIO) detachIO->start(4000);
                        process->start(exec, args);
                        ret = process->waitForStarted(250);
                        if (ret) {
                            process->waitForReadyRead(1000);
                            process->write(m_input->text().toLocal8Bit());
                            process->closeWriteChannel();
                        }
                    }
                }
                if (!ret)
                    process->deleteLater();
                return ret;
            }
        }
        if (ret) {
            if (type < ForceOut) // ForceOut, Math and List means the user waits for a response
                m_autoHide.start(3000);
            m_history.removeAll(m_input->text());
            m_history.append(m_input->text());
            if (m_history.size() > 1000)
                m_history.removeFirst();
            m_currentHistoryIndex = -1;
            if (m_historySaver) {
                if (m_historySaver->remainingTime() < 4*m_historySaver->interval()/5) { // when the timer has 80% left, we just let it run out
                    static int bumpCounter = 0;
                    if (++bumpCounter > 8) { // we've bumped this for maximum half an hour now
                        bumpCounter = 0;
                        writeHistory(); // so save the history
                    } else { // delay
                        m_historySaver->start();
                    }
                }
            }
            return true;
        }
    }
    // ============================================================================================================================

    // last resort: is this some math?  ===========================================================================================
    if (!ret) {
        if (m_qalc.isNull()) {
            if (m_bins->stringList().contains("qalc"))
                m_qalc = "qalc -f -";
            else if (m_bins->stringList().contains("bc"))
                m_qalc = "bc -ilq";
        }
        if (!m_qalc.isEmpty()) {
            process->startCommand(m_qalc);
            ret = process->waitForStarted(250);
            if (ret) {
                process->setProperty("qiq_type", "math");
                process->write(command.toLocal8Bit());
                process->closeWriteChannel();
            }
        }
    }
    if (!ret)
        process->deleteLater();
    return ret;
}

QString Qiq::filterCustom(const QString source, const QString action, const QString fieldSeparator) {
    m_externCmd = action;
    if (!m_external)
        m_external = new QStandardItemModel(this);
    m_external->clear();
    QStringList items;
    QFile f(source);
    if (f.exists()) {
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            items = QString::fromLocal8Bit(f.readAll()).split('\n');
        } else {
            return QString();
        }
    } else {
        QProcess p(this);
        p.startCommand(source);
        if (p.waitForFinished()) {
            items = QString::fromLocal8Bit(p.readAllStandardOutput()).split('\n');
        } else {
            return QString();
        }
    }
    for (const QString &s : items) {
        QStandardItem *item;
        if (fieldSeparator.isEmpty()) {
            item = new QStandardItem(s);
        } else {
            QStringList sl = s.split(fieldSeparator);
            item = new QStandardItem(sl.at(0));
            if (sl.size() > 1)
                item->setData(sl.at(0), AppExec);
        }
        m_external->appendRow(item);
    }
    m_input->clear();
    m_list->setModel(m_external);
    setCurrentWidget(m_list);
    filter(QString(), Partial);
//    ajdustGeometry();
    m_wasVisble = isVisible();
    show();
    activateWindow();
    if (m_externCmd.startsWith("%print")) {
        m_externalReply = QString();
        QElapsedTimer time;
        while (m_externalReply.isNull()) {
            time.start();
            QApplication::processEvents();
            QThread::msleep(33-time.elapsed()); // maintain 30fps but don't live-lock in processEvents()
        }
        return m_externalReply;
    }
    return QString();
}

void Qiq::toggle() {
    if (!isVisible()) {
        show();
        activateWindow();
        raise();
    } else if (isActiveWindow()) {
        hide();
    } else {
        activateWindow();
        raise();
    }
}

void Qiq::writeHistory() {
    if (m_historyPath.isEmpty() || m_history.isEmpty()) // also don't try to save an empty history - who knows what happened there
        return;
    QFile f(m_historyPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(m_history.join('\n').toUtf8());
    } else {
        qDebug() << "could not open" << m_historyPath << "for writing";
    }
}