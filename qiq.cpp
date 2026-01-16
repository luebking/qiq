/*
 *   Qiq shell for Qt6
 *   Copyright 2025 by Thomas Lübking <thomas.luebking@gmail.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License version 2
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details
 *
 *   You should have received a copy of the GNU General Public
 *   License along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <QtEnvironmentVariables>
#include <QtDBus/QDBusConnection>

#include <unistd.h>

#include <QtDebug>

#include "gauge.h"
#include "notifications.h"
#include "qiq.h"

static QRegularExpression whitespace("[;&|[:space:]]+"); //[^\\\\]*


Qiq::Qiq(bool argb) : QStackedWidget() {
    if (argb)
        setAttribute(Qt::WA_TranslucentBackground);
    QDBusConnection::sessionBus().registerService("org.qiq.qiq");
    QDBusConnection::sessionBus().registerObject("/", this);
    new DBusAdaptor(this);
//    qEnvironmentVariable(const char *varName, const QString &defaultValue)
    addWidget(m_status = new QWidget);

    m_notifications = new Notifications(argb);

    m_bins = nullptr;
    m_external = nullptr;
    m_cmdCompleted = nullptr;
    m_historySaver = nullptr;
    m_todoSaver = nullptr;
    m_todoDirty = false;
    m_todoSaved = true;
    m_selectionIsSynthetic = false;
    m_askingQuestion = false;

    m_inotify = new QFileSystemWatcher(this);
    connect(m_inotify, &QFileSystemWatcher::fileChanged, [=](const QString &path) {
        // the stylesheet is supposed to be the only tracked file, the rest are dirs
        QFile sheet(path);
        if (sheet.exists() && sheet.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qApp->setStyleSheet(QString::fromLocal8Bit(sheet.readAll()));
            sheet.close();
        }
    });

    QTimer *binlistUpdater = new QTimer(this);
    binlistUpdater->setSingleShot(true);
    connect(binlistUpdater, &QTimer::timeout, this, &Qiq::updateBinaries);
    m_inotify->addPaths(qEnvironmentVariable("PATH").split(':'));
    connect(m_inotify, &QFileSystemWatcher::directoryChanged, [=](const QString &path) {
        if (qEnvironmentVariable("PATH").split(':').contains(path))
            binlistUpdater->start(5000);
    });

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
        if (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) {
            m_input->setText(m_input->text() + " ");
            insertToken(false);
        } else {
            insertToken(false);
            runInput();
        }
    });

    addWidget(m_disp = new QTextBrowser);
    m_disp->setFrameShape(QFrame::NoFrame);
    m_disp->setFocusPolicy(Qt::NoFocus);
    m_disp->document()->setDefaultStyleSheet("a{text-decoration:none;} hr{border-color:#666;}");
//    m_disp->setFocusPolicy(Qt::ClickFocus);

    m_pwd = new QLabel(this);
    m_pwd->setObjectName("PWD_LABEL");

    m_input = new QLineEdit(this);
    connect(this, &QStackedWidget::currentChanged, [=]() {
        adjustGeometry();
        m_pwd->raise();
        m_input->raise();
        if (currentWidget() == m_list)
            connect(m_input, &QLineEdit::textEdited, this, &Qiq::filterInput, Qt::UniqueConnection);
        else
            disconnect(m_input, &QLineEdit::textEdited, this, &Qiq::filterInput);
    });

    addWidget(m_todo = new QTextEdit);
    m_todo->setObjectName("TODO");
    m_todo->setToolTip(tr("<h2>Noteboook</h2>"
        "Lines starting with a time/date before a \"|\" will automatically add reminders<br>"
        "Examples:<ul>"
        "<li>9:15 | Meeting</li>"
        "<li>1pm | lunch</li>"
        "<li>Friday | Happy Hour</li>"
        "<li>24. 12. | Christkind</li>"
        "<li>12/26 | Boxing Day</li>"
        "<li>13. Januar | Knut</li>"
        "</ul>"));
//    m_todo->setAutoFormatting(QTextEdit::AutoAll);
    m_todo->setFocusPolicy(Qt::ClickFocus);
    connect(m_todo, &QTextEdit::textChanged, [=]() { m_todoDirty = true; });
    QAction *act = new QAction(m_todo);
    act->setShortcut(Qt::Key_Escape);
    connect(act, &QAction::triggered, [=]() {
        setCurrentWidget(m_status);
        if (m_todoDirty) {
            m_todoSaved = false;
            updateTodoTimers();
            if (m_todoSaver) {
                if (m_todoSaver->remainingTime() < 4*m_todoSaver->interval()/5) { // when the timer has 80% left, we just let it run out
                    static int bumpCounter = 0;
                    if (++bumpCounter > 4) { // we've bumped this for maximum 15 minutes now
                        bumpCounter = 0;
                        writeTodoList(); // so save the history
                    } else { // delay
                        m_todoSaver->start();
                    }
                }
            }
            m_todoDirty = false;
        }
        });
    m_todo->addAction(act);

    reconfigure();
    setPwd(QDir::currentPath()); // update for colors, maybe later start dir

    if (!m_historyPath.isEmpty()) {
        QFile f(m_historyPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            m_history = QString::fromUtf8(f.readAll()).split('\n');
        } else {
            qDebug() << "could not open" << m_historyPath << "for reading";
        }
    }
    if (!m_todoPath.isEmpty()) {
        QFile f(m_todoPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            m_todo->setPlainText(QString::fromUtf8(f.readAll()));
            updateTodoTimers();
            m_todoDirty = false;
            m_todoSaved = true;
        } else {
            qDebug() << "could not open" << m_todoPath << "for reading";
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

    m_applications = new QStandardItemModel;
    QTimer *applicationUpdater = new QTimer(this);
    applicationUpdater->setSingleShot(true);
    connect(applicationUpdater, &QTimer::timeout, this, &Qiq::makeApplicationModel);
    m_inotify->addPaths(QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation));
    connect(m_inotify, &QFileSystemWatcher::directoryChanged, [=](const QString &path) {
        if (QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation).contains(path))
            applicationUpdater->start(5000);
    });

    makeApplicationModel();
    m_files = new QFileSystemModel(this);
    m_files->setFilter(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::AllDirs|QDir::Hidden|QDir::System);
    m_files->setIconProvider(new QFileIconProvider);
    m_cmdHistory = new QStringListModel(this);

    setUpdatesEnabled(false);
    show();
    message("dummy"); // QTextBrowser needs a kick in the butt, cause the first document size isn't properly calculated (at all)
    setCurrentWidget(m_status);
    adjustGeometry();
    raise();
    setUpdatesEnabled(true);
    connect(m_input, &QLineEdit::textChanged, [=](const QString &t) {
        QString text = t;
        if (text.isEmpty()) {
            m_notifications->preview(text); // hide
            m_input->hide();
            if (currentWidget() == m_list && (m_list->model() == m_external || m_list->model() == m_notifications->model()))
                return;
            if (currentWidget() != m_disp)
                setCurrentWidget(m_status);
            return;
        }
        if (text.size() > 4 && text.startsWith("qiq ")) {
            static const QString qiq_reconfigure("qiq reconfigure");
            static const QString qiq_countdown("qiq countdown [<msg>] <t>");
            m_input->blockSignals(true);
            if (qiq_reconfigure.startsWith(text)) {
                int pos = m_input->cursorPosition();
                m_input->setText(qiq_reconfigure);
                m_input->setSelection(pos, qiq_reconfigure.size()-pos);
                text = qiq_reconfigure;
            } else if (qiq_countdown.startsWith(text)) {
                m_input->setText(qiq_countdown);
                m_input->setSelection(14, qiq_countdown.size()-14);
                text = qiq_countdown;
            }
            m_input->blockSignals(false);
        }
        if (text == "cd ") {
            m_input->blockSignals(true);
            text = "cd " + QDir::currentPath();
            m_input->setText(text);
            m_input->setSelection(3, text.size()-3);
            m_input->blockSignals(false);
            explicitlyComplete();
        }
        QFont fnt = font();
        fnt.setPointSize((2.0f-qMin(1.2f, qMax(0, text.size()-24)/80.0f))*fnt.pointSize());
        m_input->setFont(fnt);
        if (currentWidget() == m_status && text.size() == 1) {
            setModel(m_applications);
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

void Qiq::updateTodoTimers() {
    for (const QTimer *t : m_todoTimers)
        delete t;
    m_todoTimers.clear();
    QStringList lines = m_todo->toPlainText().split('\n');
    for (const QString &line : lines) {
        const int pipe = line.indexOf('|');
        if (pipe < 0)
            continue;
        static const QRegularExpression bullet("^\\s*(\\*|\\+|-|·|°)\\s");
        QString head = line.left(pipe).remove(bullet).trimmed();

        QStringList tokens = head.split(QRegularExpression("\\s"), Qt::SkipEmptyParts);
        static const QRegularExpression hmm("\\d{1,2}:\\d\\d");
        static const QRegularExpression MD("\\d{1,2}/\\d{1,2}");
        static const QRegularExpression nth("st|nd|rd|th");
        int hour(-1), minute(-1), day(0), month(0), weekday(0);
        QRegularExpressionMatch match;
        for (const QString &t : tokens) {
            if (hour < 0 && minute < 0 && t.contains(':')) { // time ?
                match = hmm.match(t);
                if (match.hasMatch()) {
                    hour = match.captured(0).section(':', 0, 0).toInt();
                    if (hour > 24) // 24:00 is probably a thing
                        hour = -1;
                    minute = match.captured(0).section(':', 1, 1).toInt();
                    if (minute > 59)
                        minute = -1;
                if (hour > 0 && hour < 13 && t.endsWith(QLocale::system().pmText(), Qt::CaseInsensitive))
                    hour += 12;
                continue;
                }
            }
            if (!day && !month && t.contains('/')) { // US date ?
                match = MD.match(t);
                if (match.hasMatch()) {
                    month = match.captured(0).section('/', 0, 0).toInt();
                    day = match.captured(0).section('/', 1, 1).toInt();
                    if (month > 12 || day > 31)
                        month = day = 0;
                    continue;
                }
                QString s = t.section('/', 1, 1);
                if (s.last(2).contains(nth))
                    s.chop(2);
                bool ok;
                day = s.toInt(&ok);
                if (!ok || day > 31) {
                    day = 0;
                    continue;
                }
                for (int i = 1; i < 13; ++i) {
                    if (t.startsWith(QLocale::system().monthName(i, QLocale::ShortFormat).remove('.'))) {
                        month = i; break;
                    }
                }
                if (!month)
                    day = 0;
                continue;
            }
            if ((!day || !month) && t.endsWith('.')) { // EUR day or month
                bool ok;
                int n = t.left(t.size()-1).toInt(&ok);
                if (ok) {
                    if (!day && n < 32)
                        day = n;
                    else if (day && n < 13)
                        month = n;
                }
                continue;
            }
            if (t.endsWith(QLocale::system().amText(), Qt::CaseInsensitive)) {
                bool ok;
                int n = t.left(t.size()-QLocale::system().amText().size()).toInt(&ok);
                if (ok && n < 13)
                    hour = n;
                continue;
            }
            if (t.endsWith(QLocale::system().pmText(), Qt::CaseInsensitive)) {
                bool ok;
                int n = t.left(t.size() - QLocale::system().pmText().size()).toInt(&ok);
                if (ok && n < 13)
                    hour = n + 12;
                continue;
            }
            if (!weekday) {
                for (int i = 1; i < 8; ++i) {
                    if (t.startsWith(QLocale::system().dayName(i, QLocale::ShortFormat).remove('.'))) {
                        weekday = i; break;
                    }
                }
                if (weekday)
                    continue;
            }
            if (!month) {
                for (int i = 1; i < 13; ++i) {
                    if (t.startsWith(QLocale::system().monthName(i, QLocale::ShortFormat).remove('.'))) {
                        month = i; break;
                    }
                }
                if (month)
                    continue;
            }
            if (!day && t.last(2).contains(nth)) {
                bool ok;
                day = t.chopped(2).toInt(&ok);
                if (!ok || day > 31)
                    day = 0;
                continue;
            }
        }
        if (hour < 0 && minute < 0 && !day && !month && !weekday)
            continue; // not a date after all
        QTime t;
        if (hour > -1)
            t.setHMS(hour, 0, 0);
        if (minute > -1)
            t.setHMS(t.hour(), minute, 0);
        if (!t.isValid())
            t.setHMS(9, 30, 0);
        QDate d = QDate::currentDate();
        if (!day && weekday) {
            int days = weekday - d.dayOfWeek();
            if (days < 0)
                days += 7;
            d = d.addDays(days);
        }
        if (day)
            d.setDate(d.year(), d.month(), day);
        if (month)
            d.setDate(d.year(), month, d.day());
        else if (day && d < QDate::currentDate())
            d = d.addMonths(1); // next month
        QDateTime dt(d, t);
        if (dt < QDateTime::currentDateTime()) {
           if (!day && !month && !weekday)
                dt = dt.addDays(1);
           else if (QDateTime::currentDateTime().msecsTo(dt) < 86400000)
               dt = dt.addYears(1);
       }
       QTimer *timer = new QTimer(this);
       m_todoTimers << timer;
       timer->setSingleShot(true);
       timer->setProperty("summary", tr("Qiq reminder: ") + head);
       timer->setProperty("body", line.mid(pipe + 1).trimmed());
       connect (timer, &QTimer::timeout, [=]() {
           notifyUser(timer->property("summary").toString(), timer->property("body").toString());
           m_todoTimers.removeAll(timer);
           timer->deleteLater();
       });
       timer->start(QDateTime::currentDateTime().msecsTo(dt));
//        qDebug() << "=>" << dt << QDateTime::currentDateTime().msecsTo(dt);
//        qDebug() << hour << minute << day << month << weekday;
    }
}

void Qiq::updateBinaries() {
    const QStringList paths = qEnvironmentVariable("PATH").split(':');
    QStringList binaries;
    for (const QString &s : paths)
        binaries.append(QDir(s).entryList(QDir::Files|QDir::Executable));
    binaries.append(m_aliases.keys());
    binaries.removeDuplicates();
    binaries.sort();
    if (!m_bins)
        m_bins = new QStringListModel(binaries);
    else
        m_bins->setStringList(binaries);
}

void Qiq::reconfigure() {
    QSettings settings("qiq");
    const QString sheetPath = QStandardPaths::locate(QStandardPaths::AppDataLocation, settings.value("Style", "default.css").toString());
    QFile sheet(sheetPath);
    if (sheet.exists() && sheet.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qApp->setStyleSheet(QString::fromLocal8Bit(sheet.readAll()));
        m_inotify->removePaths(m_inotify->files()); // the style is the only file we track, discard in case it changed
        m_inotify->addPath(sheetPath);
    }
    sheet.close();

    m_aha = settings.value("AHA").toString();
    m_qalc = settings.value("CALC").toString();
    m_term = settings.value("TERMINAL", qEnvironmentVariable("TERMINAL")).toString();
    m_cmdCompletion = settings.value("CmdCompleter").toString();
    m_cmdCompletionSep = settings.value("CmdCompletionSep").toString();
    m_previewCmds = settings.value("PreviewCommands").toStringList();
    m_historyPath = settings.value("HistoryPath").toString();
    if (!m_historyPath.isEmpty() && !m_historySaver) {
        m_historySaver = new QTimer(this);
        m_historySaver->setSingleShot(true);
        m_historySaver->setInterval(300000); // 5 minutes
        connect(m_historySaver, &QTimer::timeout, this, &Qiq::writeHistory);
    }
    m_todoPath = settings.value("TodoPath",
                QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QDir::separator() + "todo.txt").toString();
    if (!m_todoPath.isEmpty() && !m_todoSaver) {
        m_todoSaver = new QTimer(this);
        m_todoSaver->setSingleShot(true);
        m_todoSaver->setInterval(300000); // 5 minutes
        connect(m_todoSaver, &QTimer::timeout, this, &Qiq::writeTodoList);
    }
    m_notifications->setOffset(settings.value("NotificationOffset", QPoint(-32,32)).toPoint());
    QStringList wmHacks = settings.value("WMHacks", "Bypass").toStringList();
    Qt::WindowFlags flags = Qt::Window;
    if (wmHacks.contains("bypass", Qt::CaseInsensitive))
        flags |= Qt::BypassWindowManagerHint;
    else
        flags |= Qt::WindowStaysOnTopHint|Qt::FramelessWindowHint;
    if (wmHacks.contains("popup", Qt::CaseInsensitive))
        flags |= Qt::Popup;
    setWindowFlags(flags);
    releaseKeyboard();
    m_grabKeyboard = wmHacks.contains("grabkeyboard", Qt::CaseInsensitive);
    if (m_grabKeyboard && isVisible())
        grabKeyboard();

    QFont gaugeFont = QFont(settings.value("GaugeFont").toString());
    QStringList gauges = settings.value("Gauges").toStringList();
    const QSize oldDefaultSize = m_defaultSize;
    m_defaultSize = QSize(settings.value("Width", 640).toUInt(), settings.value("Height", 320).toUInt());
    if (oldDefaultSize != m_defaultSize)
        resize(m_defaultSize);
    m_iconSize = settings.value("IconSize", 48).toUInt();
    m_list->setIconSize(QSize(m_iconSize,m_iconSize));
    QList<Gauge*> oldGauges = m_status->findChildren<Gauge*>();
    static QHash<QString,uint> gaugeNotificationIDs;
    for (const QString &gauge : gauges) {
        settings.beginGroup(gauge);
        Gauge *g = m_status->findChild<Gauge*>(gauge);
        if (g) {
            oldGauges.removeAll(g);
        } else {
            g = new Gauge(m_status);
            g->setObjectName(gauge);
            connect (g, &Gauge::critical, [=](const QString &m, int i) {
                const QString key = g->objectName() + QString("/%1").arg(i);
                gaugeNotificationIDs[key] = notifyUser(m, QString(), 2, gaugeNotificationIDs.value(key, 0));
            });
            connect (g, &Gauge::uncritical, [=](int i) {
                m_notifications->purge(gaugeNotificationIDs.value(g->objectName() + QString("/%1").arg(i), 0));
            });
        }

        g->setFont(gaugeFont);
        for (int i = 0; i < 3; ++i) {
            g->setSource(settings.value(QString("Source%1").arg(i+1)).toString(), i);
            g->setRange(settings.value(QString("Min%1").arg(i+1), 0).toInt(),
                        settings.value(QString("Max%1").arg(i+1), 100).toInt(), i);
            g->setColors(settings.value(QString("ColorLow%1").arg(i+1)).value<QColor>(),
                         settings.value(QString("ColorHigh%1").arg(i+1)).value<QColor>(), i);

            QString thresh = settings.value(QString("Threshold%1").arg(i+1)).toString();
            g->setCriticalThreshold(-1, Gauge::None, QString(), i);
            bool ok = false; int v;
            if (thresh.size() > 1)
                v = thresh.mid(1,-1).toInt(&ok);
            if (ok && thresh.at(0) == '>')
                g->setCriticalThreshold(v, Gauge::Maximum, settings.value(QString("ThreshMsg%1").arg(i+1)).toString(), i);
            if (ok && thresh.at(0) == '<')
                g->setCriticalThreshold(v, Gauge::Minimum, settings.value(QString("ThreshMsg%1").arg(i+1)).toString(), i);
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
        g->setThresholdsRedundant(settings.value("RedundantThresholds", false).toBool());
        settings.endGroup();
    }

    for (Gauge *og : oldGauges)
        og->deleteLater();

    m_aliases.clear();
    settings.beginGroup("Aliases");
    for (const QString &key : settings.childKeys())
        m_aliases.insert(key, settings.value(key).toString());
    settings.endGroup();
    updateBinaries();

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
    m_applications->clear();
    static QString cachePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QDir::separator() + "apps." + de_DE + ".cache";
    const QStringList paths = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    QFileInfo cacheInfo(cachePath);
    bool useCache = false;
    if (cacheInfo.exists()) {
        useCache = true;
        for (const QString &path : paths) {
            if (QFileInfo(path).lastModified() > cacheInfo.lastModified()) {
                useCache = false;
                break;
            }
        }
    }
    QSettings cache(cachePath, QSettings::IniFormat);
    QPixmap dummyPix(m_iconSize,m_iconSize);
    dummyPix.fill(Qt::transparent);
    QIcon dummyIcon(dummyPix);
    if (useCache) {
        for (const QString &entry : cache.childGroups()) {
            cache.beginGroup(entry);
            QStandardItem *item = new QStandardItem(QIcon::fromTheme(cache.value("Icon").toString(), dummyIcon), cache.value("Name").toString());
            item->setData(cache.value("Exec").toString(), AppExec);
            item->setData(cache.value("Comment").toString(), AppComment);
            item->setData(cache.value("Path"), AppPath);
            item->setData(cache.value("Terminal", false).toBool(), AppNeedsTE);
            item->setData(cache.value("Categories").toString().split(';'), AppCategories);
            item->setData(cache.value("Keywords").toString().split(';'), AppKeywords);
            m_applications->appendRow(item);
            cache.endGroup();
        }
        return;
    }
    cache.clear();
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
            cache.beginGroup(file);
            cache.setValue("Name", name);
            cache.setValue("Exec", exec);
            QString icon = service.value("Icon").toString();
            if (!icon.isEmpty())
                cache.setValue("Icon", icon);
            QStandardItem *item = new QStandardItem(QIcon::fromTheme(icon, dummyIcon), name);
            item->setData(exec, AppExec);
            //-----
            LOCAL_AWARE(comment, "Comment")
            if (!comment.isEmpty())
                cache.setValue("Comment", comment);
            item->setData(comment, AppComment);
            //-----
            QVariant v = service.value("Path");
            if (v.isValid()) {
                cache.setValue("Path", v);
                item->setData(v, AppPath);
            }
            //-----
            v = service.value("Terminal");
            bool terminal = false;
            if (v.isValid()) {
                terminal = v.toBool();
                cache.setValue("Terminal", terminal);
            }
            item->setData(terminal, AppNeedsTE);
            //-----
            QString cats = service.value("Categories").toString();
            if (!cats.isEmpty())
                cache.setValue("Categories", cats);
            item->setData(cats.split(';'), AppCategories);
            //-----
            LOCAL_AWARE(keywords, "Keywords")
            if (!keywords.isEmpty())
                cache.setValue("Keywords", keywords);
            item->setData(keywords.split(';'), AppKeywords);
            ///@todo mimetype ??
            cache.endGroup();
            m_applications->appendRow(item);
        }
    }
}

uint Qiq::notifyUser(const QString &summary, const QString &body, int urgency, uint id) {
    QVariantMap hints;
    hints["transient"] = true;
    if (urgency != 1)
        hints["urgency"] = urgency;
    return m_notifications->add("Qiq", id, "qiq", summary, body, QStringList(), hints, 0);
}

void Qiq::adjustGeometry() {
    if (currentWidget() != m_disp) {
        m_disp->setMinimumSize(QSize(0,0));
        setMinimumSize(QSize(0,0));
    }
    if (currentWidget() == m_disp) {
        QSize max(800,800);
        if (const QScreen *screen = windowHandle()->screen()) {
            max = screen->geometry().size()*0.666666667;
        }
        m_disp->setMinimumWidth(qMax(m_defaultSize.width(), max.width()));
        if (m_disp->document()->idealWidth() < 0.75*m_disp->minimumWidth())
            m_disp->setMinimumWidth(qMax(m_defaultSize.width(), 12+qCeil(m_disp->document()->idealWidth())));
        m_disp->setMinimumHeight(qMin(max.height(), 12+qCeil(m_disp->document()->size().height())));
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
    activateWindow(); // we might lose the mouse and the WM might withdraw the focus
}

class CmdComplDelegate : public QStyledItemDelegate {
    public:
        CmdComplDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
        QString displayText(const QVariant &value, const QLocale &locale) const {
            Q_UNUSED(locale)
            return value.toString().section(separator, 0, 0);
        }
        void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
            QStyledItemDelegate::paint(painter, option, index);
            const QString comment = index.data().toString().section(separator, 1, -1);
            if (comment.isEmpty())
                return;
            QColor oc = painter->pen().color(), c = oc;
            c.setAlpha(c.alpha()/2);
            painter->setPen(c);
            QRect r(option.rect);
            r.setX(r.x()+r.width()/3);
            painter->drawText(r, comment);
            painter->setPen(oc);
        }
        QString separator;
};

void Qiq::setModel(QAbstractItemModel *model) {
    static QFont monospace("monospace");
    static QAbstractItemDelegate *mainDelegate = nullptr;
    m_list->setModel(model);
    if (model == m_applications)
        m_list->setIconSize(QSize(m_iconSize,m_iconSize));
    else
        m_list->setIconSize(QSize());
    if (model == m_applications) {
        QFont fnt;
        fnt.setPointSize(1.25*fnt.pointSize());
        m_list->setFont(fnt);
    } else if (model == m_notifications->model()) {
         m_list->setFont(QFont());
    } else {
        m_list->setFont(monospace);
    }
    if (model == m_cmdCompleted) {
        static CmdComplDelegate *cmdComplDelegate = new CmdComplDelegate(this);
        cmdComplDelegate->separator = m_cmdCompletionSep;
        if (m_list->itemDelegate() != cmdComplDelegate) {
            mainDelegate = m_list->itemDelegate();
            m_list->setItemDelegate(cmdComplDelegate);
        }
    } else if (mainDelegate) {
        m_list->setItemDelegate(mainDelegate);
    }
}

void Qiq::setPwd(QString path) {
    QDir::setCurrent(path);
    path.replace(QDir::homePath(), "~");
    m_pwd->setText(m_pwd->fontMetrics().elidedText(path, Qt::ElideLeft, width()/4-32));
    m_pwd->setToolTip(path == m_pwd->text() ? QString() : path);
    m_pwd->adjustSize();
    m_pwd->move(width() - m_pwd->width() - 32, height() - m_pwd->height() - 16);
}

bool Qiq::event(QEvent *event) {
    static QString wmscript = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QDir::separator() + "wmhacks";
    if (event->type() == QEvent::Resize) {
        m_pwd->move(width() - m_pwd->width() - 32, height() - m_pwd->height() - 16);
    } else if (event->type() == QEvent::Show) {
        if (m_grabKeyboard)
            grabKeyboard();
        QProcess::startDetached(wmscript, QStringList() << "show");
    } else if (event->type() == QEvent::Hide) {
        if (m_grabKeyboard)
            releaseKeyboard();
        QProcess::startDetached(wmscript, QStringList() << "hide");
    } else if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        if (m_grabKeyboard)
            grabKeyboard();
        QProcess::startDetached(wmscript, QStringList() << "activate");
    }
    return QStackedWidget::event(event);
}

void Qiq::enterEvent(QEnterEvent *ee) {
    m_autoHide.stop(); // user interaction
    activateWindow();
    QStackedWidget::enterEvent(ee);
}

static QString previousNeedle;
bool Qiq::eventFilter(QObject *o, QEvent *e) {
    if (o == m_input && e->type() == QEvent::KeyPress) {
        m_autoHide.stop(); // user interaction
        const int key = static_cast<QKeyEvent*>(e)->key();
        if (key == Qt::Key_Tab) {
            if (m_input->text().isEmpty()) {
                if (currentWidget() == m_status) {
                    setModel(m_applications);
                    filter(QString(), Partial);
                    setCurrentWidget(m_list);
                } else if (currentWidget() == m_list) {
                    if (m_list->model() == m_applications) {
                        setModel(m_bins);
                        filter(QString(), Begin);
                    } else if (m_list->model() == m_bins) {
                        setModel(m_external);
                        filter(QString(), Partial);
                    } else if (m_list->model() == m_external) {
                        setModel(m_applications);
                        setCurrentWidget(m_disp);
                    }
                } else if (currentWidget() == m_disp) {
                    setCurrentWidget(m_status);
                }
            } else if (m_selectionIsSynthetic && m_input->selectionEnd() > -1) {
                int newPos = m_input->selectionEnd();
                if (m_list->model() == m_files && m_input->text().at(newPos-1) == '"')
                    --newPos;
                m_input->deselect();
                m_selectionIsSynthetic = false;
                m_input->setCursorPosition(newPos);
            } else {
                explicitlyComplete();
            }
            return true;
        }
        if ((key == Qt::Key_PageUp || key == Qt::Key_PageDown) && currentWidget() == m_list) {
            m_list->setEnabled(true);
            QApplication::sendEvent(currentWidget(), e);
            insertToken(false);
            return true;
        }
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            if (currentWidget() == m_list) {
                m_list->setEnabled(true);
                QApplication::sendEvent(currentWidget(), e);
                insertToken(false);
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
            if (m_askingQuestion) {
                m_askingQuestion = false;
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
            } else if (currentWidget() == m_todo) {
                /// @todo parse and save todo
                setCurrentWidget(m_status);
            } else { //QApplication::quit();
                m_externalReply = QString(""); // empt, not null!
                hide();
            }
            return true;
        }
        if (key == Qt::Key_Space || (m_list->model() == m_files && static_cast<QKeyEvent*>(e)->text() == "/")) {
            if (m_selectionIsSynthetic && m_input->selectionEnd() > -1) {
                int newPos = m_input->selectionEnd();
                if (key != Qt::Key_Space && m_input->text().at(newPos-1) == '"') // key != Qt::Key_Space implies "/" on files
                    --newPos;
                m_input->deselect();
                m_selectionIsSynthetic = false;
                m_input->setCursorPosition(newPos);
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
            setModel(m_cmdHistory);
            filter(m_inputBuffer, Partial);
            setCurrentWidget(m_list);
            return true;
        }
        if (key == Qt::Key_N && (static_cast<QKeyEvent*>(e)->modifiers() & Qt::ControlModifier)) {
            m_input->clear();
            setModel(m_notifications->model());
            filter(QString(), Partial);
            setCurrentWidget(m_list);
            return true;
        }
        if (key == Qt::Key_T && (static_cast<QKeyEvent*>(e)->modifiers() & Qt::ControlModifier)) {
            m_input->clear();
            setCurrentWidget(m_todo);
            m_todo->setFocus();
            return true;
        }
        if (key == Qt::Key_Delete && currentWidget() == m_list &&
            // only if the user doesn't want to delete texr
            !m_input->selectionLength() && m_input->cursorPosition() == m_input->text().size() &&
            // … and valid indices
            m_list->currentIndex().isValid()) {
            if (m_list->model() == m_cmdHistory) {
                m_cmdHistory->removeRows(m_list->currentIndex().row(), 1);
                m_history.removeAll(m_list->currentIndex().data().toString());
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
        insertToken(false);
        return;
    }
    if (currentWidget() == m_list && m_list->model() == m_cmdHistory) {
        cycleResults = true;
        insertToken(false);
        return;
    }
    QString path = lastToken;
    if (path.startsWith('~'))
        path.replace(0,1,QDir::homePath());
    QFileInfo fileInfo(path);
    QDir dir = fileInfo.dir();
    if (!dir.exists() && path.startsWith('"')) {
        fileInfo = QFileInfo(path.mid(1,path.size()-1-path.endsWith('"')));
        dir = fileInfo.dir();
    }

    auto completeDir = [=](const QDir &cdir, bool force) {
        setCurrentWidget(m_list);
        setModel(m_files);
        bool delayed = false;
        if (m_files->rootPath() != cdir.absolutePath()) {
            delayed = true;
            // this can take a moment to feed the model
            connect(m_files, &QFileSystemModel::directoryLoaded, this, [=](const QString &path) {
                            if (path == m_files->rootPath()) {
                                m_files->sort(0);
                                filter(fileInfo.fileName(), Begin);
                                insertToken(true);
                            }
                            cycleResults = true;
                            }, Qt::SingleShotConnection);
            m_files->setRootPath(cdir.absolutePath());
            force = true;
        }
        if (force) {
            QModelIndex newRoot = m_files->index(m_files->rootPath());
            m_list->setCurrentIndex(QModelIndex());
            m_list->setRootIndex(newRoot);
            previousNeedle.clear();
            if (!delayed) {
                filter(fileInfo.fileName(), Begin);
            }
        }
        if (!delayed)
            insertToken(true);
        cycleResults = true; // last because reset by filtering
    };

    if (dir.exists() && (dir != QDir::current() || lastToken.contains('/'))) {
        completeDir(dir, false);
        return;
    }
    auto stripInstruction = [=](QString &token) {
        if (token.startsWith('=') || token.startsWith('?') || token.startsWith('!') || token.startsWith('#'))
            token.remove(0,1);
    };
    QString lastCmd = m_input->text().left(m_input->cursorPosition()).section(" | ", -1, -1);
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
                if (!completions.isEmpty() && completions.constLast().isEmpty())
                    completions.removeLast();
                if (!completions.isEmpty() && completions.constFirst().startsWith("__files"/*\r*/)) {
                    completeDir(QDir::current(), true);
                    return;
                }
                completions.removeDuplicates();
                m_cmdCompleted->setStringList(completions);
                setModel(m_cmdCompleted);
                setCurrentWidget(m_list);
                filterInput();
                insertToken(true);
            }
        }
    } else {
        setModel(m_bins);
        previousNeedle.clear();
        lastCmd = lastToken;
        stripInstruction(lastCmd);
        filter(lastCmd, Begin);
        setCurrentWidget(m_list);
        insertToken(true);
    }
    cycleResults = true;
}

void Qiq::tokenUnderCursor(int &left, int &right) {
    const QString text = m_input->text();
    left = text.lastIndexOf(whitespace, m_input->cursorPosition() - 1) + 1;
    right = text.indexOf(whitespace, m_input->cursorPosition());
    if (right < 0)
        right = text.length();
    if (text.at(right-1) == '"') {
        left = qMax(0, text.lastIndexOf('"', right - 2));
    }
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
        const bool filterDot = (m_list->model() == m_files) && !needle.startsWith('.');
        for (int i = 0; i < rows; ++i) {
            const QString hay = m_list->model()->index(i, 0, m_list->rootIndex()).data().toString();
            const bool vis = !(filterDot && hay.startsWith('.')) && hay.startsWith(needle, Qt::CaseInsensitive);
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
        QStandardItemModel *takeScores = nullptr;
        if (!needle.isEmpty())
            takeScores = qobject_cast<QStandardItemModel*>(m_list->model());
        for (int i = 0; i < rows; ++i) {
            QModelIndex index = m_list->model()->index(i, 0, m_list->rootIndex());
            const QString &hay = index.data().toString();
            bool vis = true;
            for (const QString &s : sl) {
                if (!hay.contains(s, Qt::CaseInsensitive)) {
                    vis = false;
                    break;
                }
            }
            if (takeScores) {
                int score = 0;
                if (vis) {
                    score = 1;
                    if (hay.startsWith(needle))
                        score = 100;
                    else if (hay.contains(needle))
                        score = 50;
                }
                takeScores->setData(index, score, MatchScore);
            }
            m_list->setRowHidden(i, !(vis && ++visible));
        }
        if (takeScores) {
            takeScores->setSortRole(MatchScore);
            takeScores->sort(0, Qt::DescendingOrder);
        }
        // sorting unfortunately trashes the order, so we need a second pass for that
        // (not special cased for the rare occasion of !takeScores)
        for (int i = 0; i < rows; ++i) {
            if (!m_list->isRowHidden(i)) {
                m_lastVisibleRow = i;
                if (firstVisRow < 0)
                    firstVisRow = i;
            }
        }
        shrink = previousNeedle.contains(needle, Qt::CaseInsensitive);
    }
    previousNeedle = needle;
    const int row = m_list->currentIndex().row();
    bool looksLikeCommand = false;
    if (m_list->currentIndex().isValid() && (m_list->model() == m_applications || m_list->model() == m_external))
        looksLikeCommand = m_input->text().contains(" | ") || (m_input->text().trimmed().contains(whitespace) && m_bins->stringList().contains(m_input->text().split(whitespace).first()));
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
        QTimer::singleShot(1, this, [=]() { Qiq::insertToken(true); }); // needs to be delayed to trigger the textChanged after the actual edit
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
        if (text.startsWith('~'))
            text.replace(0,1,QDir::homePath());
        QFileInfo fileInfo(text);
        QDir dir = fileInfo.dir();
        if (!dir.exists() && text.startsWith('"') && text.endsWith('"')) {
            fileInfo = QFileInfo(text.mid(1,text.size()-2));
            dir = fileInfo.dir();
        }
        const QString path = dir.absolutePath();
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

void Qiq::insertToken(bool selectDiff) {
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
        // preserve present token to not screw the users input
        int left, right;
        tokenUnderCursor(left, right);
        QString token = m_input->text().mid(left, right - left);
        int slash = token.lastIndexOf(QDir::separator());
        newToken = token.left(slash + 1) + newToken;
        // need canonical path fpr preview
        token = newToken;
        if (token.startsWith('~'))
            token.replace(0,1,QDir::homePath());
        for (const QString &cmd : m_previewCmds) {
            if (m_input->text().startsWith(cmd)) {
                m_notifications->preview(token);
                break;
            }
        }
        // fix quotation for command
        if (newToken.contains(whitespace)) {
            if (!newToken.startsWith("\""))
                newToken = "\"" + newToken;
            if (!newToken.endsWith("\""))
                newToken += "\"";
        } else if (newToken.startsWith("\"")) {
            newToken.remove(0,1);
        }
    } else if (m_list->model() == m_cmdHistory) {
        int pos = selectDiff ? newToken.indexOf(m_input->text()) + m_input->cursorPosition() : -1;
        m_input->setText(newToken);
        if (pos > -1)
            m_input->setSelection(pos, newToken.length());
        return;
    } else if (m_list->model() == m_cmdCompleted) {
        if (!m_cmdCompletionSep.isEmpty()) {
            newToken = newToken.section(m_cmdCompletionSep, 0, 0);
            newToken.remove('\r'); // zsh completions at times at least have that, probably to control the cursor
            newToken.remove('\t'); newToken.remove('\a'); // just for good measure
        }
    }
    QString text = m_input->text();

    int pos = selectDiff ? -1 : m_input->selectionStart();
    int cursorOffset = 0;
    if (pos > -1) {
        int len = m_input->selectionLength();
        text.replace(pos, len, newToken);
    } else {
        int left, right;
        tokenUnderCursor(left, right);
        const QChar firstChar = text.at(left);
        if (firstChar == '=' || firstChar == '?' || firstChar == '!' || firstChar == '#')
            ++left;
        if (m_list->model() == m_files && newToken.startsWith('"') && text.at(left) != '"')
            ++cursorOffset;
        text.replace(left, right - left, newToken);
        pos = -(left+newToken.size());
    }
    if (text == m_input->text())
        return; // idempotent, leave alone
    int sl = 0;
    if (pos > -1) {
        sl = newToken.length();
    } else if (selectDiff) {
        sl = -pos - m_input->cursorPosition();
        pos = m_input->cursorPosition() + cursorOffset;
    } else {
        pos = -pos;
    }
    m_input->setText(text);
    if (sl > 0) {
        m_input->setSelection(pos, sl);
        m_selectionIsSynthetic = true;
        connect(m_input, &QLineEdit::selectionChanged, this, [=]() { m_selectionIsSynthetic = false; }, Qt::SingleShotConnection);
    } else {
        m_input->setCursorPosition(pos);
    }
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
        output = "<h3 align=center style=\"color:#d01717;\">" + process->program() + " " + process->arguments().join(" ") + "</h3><pre style=\"color:#d01717;\">";
        QByteArray error = process->readAllStandardError();
        if (!error.isEmpty()) {
            output += QString::fromLocal8Bit(error).toHtmlEscaped();
        output += "</pre>";
        }
    } else {
        m_disp->setTextColor(m_disp->palette().color(m_disp->foregroundRole()));
    }
    bool showAsList = false;
    QByteArray stdout = process->readAllStandardOutput();
    if (process->property("%clip%").toBool()) {
        QString string = QString::fromLocal8Bit(stdout);
        QGuiApplication::clipboard()->setText(string, QClipboard::Clipboard);
        QGuiApplication::clipboard()->setText(string, QClipboard::Selection);
        stdout.clear();
        output += "<h3 align=center>" + tr("Copied to clipboard") + "</h3>";
    }
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
            setModel(m_external);
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

QString Qiq::ask(const QString &question, QLineEdit::EchoMode mode) {
    const bool wasVisible= isVisible();
    m_askingQuestion = true;
    QWidget *previousWidget = currentWidget();
    if (!isActiveWindow())
        toggle();
    QString q = question;
    if (!mightBeRichText(q))
        q = "<h1 align=center>" + q + "</h1>";
    message(q + "<p align=center>" + tr("(press escape to abort)") + "</p>");
    m_input->clear();
    m_input->setEchoMode(mode);
    QElapsedTimer time;
    while (m_askingQuestion) {
        time.start();
        QApplication::processEvents();
        QThread::msleep(33-time.elapsed()); // maintain 30fps but don't live-lock in processEvents()
    }
    const QString response = m_input->text();
    m_input->clear();
    m_input->setEchoMode(QLineEdit::Normal);
    if (!wasVisible)
        m_autoHide.start(250);
    setCurrentWidget(previousWidget);
    return response;
}


bool Qiq::runInput() {
    if (m_askingQuestion) {
        // got answer, fix mode and return - don't close
        m_askingQuestion = false;
        return false;
    }

    m_notifications->preview(QString()); // hide
    QAbstractItemModel *currentModel = nullptr;
    if (currentWidget() == m_list)
        currentModel = m_list->model();

    // filter from custom list ==========================================================================================================
    if (currentModel && // m_external is lazily created
        currentModel == m_external && m_externCmd != "_qiq") {
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
                QStringList args = QProcess::splitCommand(m_externCmd);
                args << v;
                ret = QProcess::startDetached(args.takeFirst(), args);
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
        bool accept = false;
        if (entry.isValid()) {
            accept = entry.data().toString() == m_input->text();
            m_input->setText(entry.data().toString());
        }
        setModel(m_bins);
        setCurrentWidget(m_status);
        m_cmdHistory->setStringList(QStringList());
        m_autoHide.stop(); // SIC! Just in case it's running
        if (!accept)
            return false; // SIC! we don't want the input to be accepted, just changed
        // otherwise fall through to command handling
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
    QFileInfo fInfo(command);
    if (!fInfo.exists() && command.startsWith("cd "))
        fInfo = QFileInfo(command.sliced(3,command.size()-3));
    if (!fInfo.exists() && fInfo.filePath().startsWith('"') && fInfo.filePath().endsWith('"'))
        fInfo = QFileInfo(fInfo.filePath().mid(1,fInfo.filePath().size()-2));
    if (fInfo.exists()) {
        if (fInfo.isDir()) {
            setPwd(fInfo.filePath());
            m_autoHide.stop(); // SIC! Just in case it's running
            return true;
        }
        m_autoHide.start(1000);
        return QProcess::startDetached("xdg-open", QStringList() << fInfo.filePath());
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

    // internal command, don't roundtrip over dbus…
    if (command.simplified() == "qiq reconfigure") {
        reconfigure();
        return true;
    }
    if (command.simplified().startsWith("qiq countdown")) {
        QStringList args = QProcess::splitCommand(command);
        if (args.count() < 3) {
            message("<h1 align=center>qiq countdown <time> [<message>]</h1>");
            return false;
        }
        args.remove(0, 2);
        const int ms = msFromString(args.takeLast());
        if (ms < 0) {
            message("<h1 align=center>Invalid time signature - try 5.30 or 5m30s</h1>");
            return false;
        }
        QString summary = args.join(" ");
        if (!summary.contains("%counter%"))
            summary.append(" %counter%");
        QVariantMap hints; hints["transient"] = true; hints["countdown"] = true;
        m_notifications->add("qiq", 0, "qiq", summary, QString(), QStringList(), hints, ms);
        return true;
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
    if (command == "%clip%") {
        process->deleteLater();
        message(QGuiApplication::clipboard()->text());
        return true;
    }

    QList<QProcess*> feeders;
    bool clipIn = false;
    if (command.contains(" | ")) {
        QStringList components = command.split(" | ", Qt::SkipEmptyParts);
        if (!components.isEmpty())
            command = components.takeLast().trimmed();
        else {
            process->deleteLater();
            message("<h3>You come from nothing, you're going back to nothing.<br>What have you lost?</h3><h1>Nothing!</h1>");
            return false;
        }
        if (command == "%clip%") {
            if (components.isEmpty()) {
                process->deleteLater();
                message("<h1>So, clip… WHAT?!?</h1>");
                return false;
            }
            type = ForceOut;
            process->setProperty("%clip%", true);
            command = components.takeLast().trimmed();
        }
        if (!components.isEmpty() && components.constFirst().trimmed() == "%clip%") {
            clipIn = true;
            components.removeFirst();
        }
        QProcess *sink = process;
        for (int i = components.size() - 1; i >= 0; --i) {
            const QString component = components.at(i).trimmed();
            QStringList args = QProcess::splitCommand(component);
            QProcess *feeder = new QProcess(this);
            feeder->setProgram(args.takeFirst());
            feeder->setArguments(args);
            connect(feeder, &QProcess::finished, feeder, &QObject::deleteLater);
            feeder->setStandardOutputProcess(sink);
            sink = feeder;
            feeders.prepend(feeder);
        }
    }

    for (QProcess *feeder : feeders) {
        feeder->start();
        if (clipIn) {
            clipIn = false;
            feeder->write(QGuiApplication::clipboard()->text().toLocal8Bit());
            feeder->closeWriteChannel();
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
        QString alias = m_aliases.value(bin, bin);
        if (alias != bin) {
            if (alias.contains("%s")) {
                alias.replace("%s", command.mid(sp+1));
                sp = command.size();
            }
            command.replace(0, sp, alias);
        }
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

        // cannot detach when writing stdin
        if (clipIn && type == NoOut)
            type = Normal;

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
                    QString password = ask("<h3 align=center>" + command + "</h3><h1 align=center>" + tr("…enter your sudo password…") + "</h1>", QLineEdit::Password);
                    if (password.isEmpty()) {
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
                            setCurrentWidget(m_status);
                            process->write(password.toLocal8Bit());
                            process->closeWriteChannel();
                        }
                    }
                }
                if (!ret) {
                    process->deleteLater();
                    return ret;
                }
                m_input->setText(command); // restore for history
            }
            if (clipIn && !isSudo) {
                clipIn = false;
                process->write(QGuiApplication::clipboard()->text().toLocal8Bit());
                process->closeWriteChannel();
            }
        }
        if (ret) {
            if (type < ForceOut) // ForceOut, Math and List means the user waits for a response
                m_autoHide.start(type == NoOut ? 250 : 3000);
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
                item->setData(sl.at(1), AppExec);
        }
        m_external->appendRow(item);
    }
    m_input->clear();
    setModel(m_external);
    setCurrentWidget(m_list);
    filter(QString(), Partial);
    m_wasVisble = isVisible();
    show();
    activateWindow();
    raise();
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
        m_wasVisble = true;
        show();
        adjustGeometry();
        raise();
        m_input->setFocus();
    } else if (isActiveWindow()) {
        hide();
    } else {
        adjustGeometry();
        raise();
        m_input->setFocus();
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

void Qiq::writeTodoList() {
    if (m_todoPath.isEmpty() || m_todoSaved) // allow deleting notes
        return;
    QFile f(m_todoPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(m_todo->toPlainText().toUtf8());
        m_todoSaved = true;
    } else {
        qDebug() << "could not open" << m_todoPath << "for writing";
    }
}

int Qiq::msFromString(const QString &string) {
    int ms = 0;
    const QRegularExpression hour("\\d+[h:]"), minute("\\d+[m\\.]"), second("\\d+s"), minute2(":\\d+"), second2("\\.\\d+");
    QRegularExpressionMatch match;
    auto matchedMs = [&match](uint factor, int sign) {
        if (match.hasMatch()) {
            bool ok;
            QString s = match.captured(0);
            if (sign == -1)
                s.removeLast();
            else if (sign == 1)
                s.removeFirst();
            int v = s.toUInt(&ok);
            if (ok)
                return v*factor;
        }
        return 0u;
    };
    match = hour.match(string);     ms += matchedMs(60*60*1000, -1);
    match = minute.match(string);   ms += matchedMs(60*1000, -1);
    match = minute2.match(string);  ms += matchedMs(60*1000, 1);
    match = second.match(string);   ms += matchedMs(1000, -1);
    match = second2.match(string);  ms += matchedMs(1000, 1);
    if (!ms) {
        bool ok;
        ms = string.toUInt(&ok);
        if (!ok) {
            return -1;
        }
    }
    return ms;
}


int DBusAdaptor::index(QString &gauge) const {
    int split = gauge.lastIndexOf('%');
    int r = gauge.mid(split+1).toUInt();
    if (r < 1 || r > 3)
        return 0;
    gauge.truncate(split);
    return r;
}

void DBusAdaptor::toggle(QString gauge, bool on) {
    if (Gauge *g = qiq->findChild<Gauge*>(gauge))
        g->toggle(on);
}

void DBusAdaptor::setLabel(QString gauge, const QString &label) {
    if (Gauge *g = qiq->findChild<Gauge*>(gauge))
        g->setLabel(label);
}
void DBusAdaptor::setRange(QString gauge, int min, int max) {
    if (int i = index(gauge)) {
    if (Gauge *g = qiq->findChild<Gauge*>(gauge))
        g->setRange(min, max, i-1);
    }
}
void DBusAdaptor::setValue(QString gauge, int value) {
    if (int i = index(gauge)) {
    if (Gauge *g = qiq->findChild<Gauge*>(gauge))
        g->setValue(value, i-1);
    }
}