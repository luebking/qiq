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

#include <unistd.h>

#include <QtDebug>

#include "gauge.h"
#include "qiq.h"

static QRegularExpression whitespace("[;&|[:space:]]+"); //[^\\\\]*

int main (int argc, char **argv)
{
    QString command = "toggle";
    QStringList parameters;
    if (argc > 1) {
        command = QString::fromLocal8Bit(argv[1]);
        for (int i = 2; i < argc; ++i)
            parameters << QString::fromLocal8Bit(argv[i]);
    }
    if (QDBusConnection::sessionBus().interface()->isServiceRegistered("org.qiq.qiq")) {
        QDBusInterface qiq( "org.qiq.qiq", "/", "org.qiq.qiq" );
        if (command == "toggle") {
            qiq.call(QDBus::NoBlock, "toggle");
            return 0;
        }
        if (command == "filter") {
            QList<QVariant> vl;
            for (const QString &s : parameters)
                vl << s;
            if (parameters.size() > 1 && parameters.at(1) == "%print%") {
                QDBusReply<QString> reply = qiq.callWithArgumentList(QDBus::Block, "filter", vl);
                if (reply.isValid())
                    printf("%s\n", reply.value().toLocal8Bit().data());
            } else {
                qiq.callWithArgumentList(QDBus::NoBlock, "filter", vl);
            }
            return 0;
        }
    }
    QStyle *style = QStyleFactory::create("Fusion"); // take some cotrol to allow predictable style sheets
    QApplication::setStyle(style);
    QApplication a(argc, argv);
    QApplication::setStyle(style);
    QCoreApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles, true);
    new Qiq;
    return a.exec();
}


Qiq::Qiq() : QStackedWidget() {
    QDBusConnection::sessionBus().registerService("org.qiq.qiq");
    QDBusConnection::sessionBus().registerObject("/", this);
    new DBusAdaptor(this);
//    qEnvironmentVariable(const char *varName, const QString &defaultValue)
    setWindowFlags(Qt::BypassWindowManagerHint);
    addWidget(m_status = new QWidget);
    QSettings settings("qiq");

    QFile sheet(QStandardPaths::locate(QStandardPaths::AppDataLocation, settings.value("Style", "default.css").toString()));
    if (sheet.exists() && sheet.open(QIODevice::ReadOnly | QIODevice::Text))
        qApp->setStyleSheet(QString::fromLocal8Bit(sheet.readAll()));
    sheet.close();

    QStringList gauges = settings.value("Gauges").toStringList();
    m_defaultSize = QSize(settings.value("Width", 640).toUInt(), settings.value("Height", 320).toUInt());
    resize(m_defaultSize);
    for (const QString &gauge : gauges) {
        settings.beginGroup(gauge);
        Gauge *g = new Gauge(this/* m_status */);
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
    
    m_external = nullptr;

    addWidget(m_list = new QListView);
    m_list->setFrameShape(QFrame::NoFrame);
//    m_list->setAutoFillBackground(false);
//    m_list->viewport()->setAutoFillBackground(false);
    m_lastVisibleRow = -1;
    m_list->setFocusPolicy(Qt::NoFocus);
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
    connect(this, &QStackedWidget::currentChanged, [=]() { adjustGeometry(); m_input->raise(); m_input->setFocus(); });
    m_input->setGeometry(0,0,0,m_input->height());
    m_input->setFrame(QFrame::NoFrame);
    m_input->setAutoFillBackground(false);
    m_input->setAlignment(Qt::AlignCenter);
    m_input->setFocus();
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
    show();
    adjustGeometry();
    activateWindow();
    connect(m_input, &QLineEdit::textChanged, [=](const QString &text) {
        if (text.isEmpty()) {
            m_input->hide();
            if (currentWidget() == m_list && m_list->model() == m_external)
                return;
            disconnect(m_input, &QLineEdit::textEdited, this, &Qiq::filterInput);
            if (currentWidget() != m_disp)
                setCurrentWidget(m_status);
            return;
        }
        if (currentWidget() == m_status && text.size() == 1) {
            m_list->setModel(m_applications);
            filterInput();
            setCurrentWidget(m_list);
            connect(m_input, &QLineEdit::textEdited, this, &Qiq::filterInput, Qt::UniqueConnection);
        }

        const QSize ts = m_input->fontMetrics().boundingRect(text).size();
        const int w = m_input->style()->sizeFromContents(QStyle::CT_LineEdit, nullptr, ts, m_input).width() + 8;
        m_input->setGeometry((width() - w)/2, (height() - m_input->height())/2, w, m_input->height());
        m_input->show();
    });
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
    if (const QScreen *screen = windowHandle()->screen()) {
        QRect r = rect();
        r.moveCenter(screen->geometry().center());
        setGeometry(r);
    } else {
        qDebug() << "fuck wayland";
    }
}

void Qiq::enterEvent(QEnterEvent *ee) {
    activateWindow();
    QStackedWidget::enterEvent(ee);
}

bool Qiq::eventFilter(QObject *o, QEvent *e) {
    if (o == m_input && e->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(e)->key();
        if (key == Qt::Key_Tab) {
            explicitlyComplete(m_input->text().left(m_input->cursorPosition()).section(whitespace, -1, -1));
            return true;
        }
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            QApplication::sendEvent(currentWidget(), e);
            insertToken();
            return true;
        }
        if (key == Qt::Key_Escape) {
            if (m_input->isVisible()) {
                m_input->clear();
            } else if (currentWidget() == m_disp || m_list->model() == m_external) {
                if (!m_wasVisble && m_list->model() == m_external)
                    hide();
                setCurrentWidget(m_status);
            } else { //QApplication::quit();
                m_externalReply = QString(""); // empt, not null!
                hide();
            }
            return true;
        }
        if (key == Qt::Key_Enter || key == Qt::Key_Return) {
            if (runInput())
                m_input->clear();
            return true;
        }
        return false;
    }
    return false;
}

static QString previousNeedle;
static bool cycleResults = false;
void Qiq::explicitlyComplete(const QString token) {
    if (cycleResults) {
        if (!m_list->currentIndex().isValid() || m_list->currentIndex().data().toString() == token) {
            QModelIndex oldIndex = m_list->currentIndex();
            QKeyEvent ke(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
            QApplication::sendEvent(m_list, &ke);
            if (oldIndex == m_list->currentIndex()) {
                QKeyEvent ke(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
                QApplication::sendEvent(m_list, &ke);
                if (m_list->isRowHidden(0)) {
                    QKeyEvent ke(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
                    QApplication::sendEvent(m_list, &ke);
                }
            }
        }
        insertToken();
        return;
    }
    QString path = token;
    if (path.startsWith('~'))
        path.replace(0,1,QDir::homePath());
    QFileInfo fileInfo(path);
    QDir dir = fileInfo.dir();
    if (dir.exists() && (dir != QDir::current() || token.contains('/'))) {
        m_list->setModel(m_files);
        m_files->setRootPath(dir.absolutePath());
        m_list->setCurrentIndex(QModelIndex());
        QModelIndex newRoot = m_files->index(m_files->rootPath());
        m_list->setRootIndex(newRoot);
        previousNeedle.clear();
        setCurrentWidget(m_list);
        // this can take a moment to feed the model
        connect(m_files, &QFileSystemModel::directoryLoaded, this, [=](const QString &path) {
                        qDebug() << path << m_files->rootPath();
                        if (path == m_files->rootPath())
                            filter(fileInfo.fileName(), Begin);
                        }, Qt::SingleShotConnection);
    } else {
        m_list->setModel(m_bins);
        previousNeedle.clear();
        filter(token, Begin);
        setCurrentWidget(m_list);
    }
    cycleResults = true;
    m_input->raise();
    connect(m_input, &QLineEdit::textEdited, this, &Qiq::filterInput, Qt::UniqueConnection);
}

void Qiq::filter(const QString needle, MatchType matchType) {
    cycleResults = false;
    if (!m_list->model())
        return;
    bool shrink = false;
    const int rows = m_list->model()->rowCount(m_list->rootIndex());
    int visible = 0;
    static int prevVisible = 0;
    int firstVisRow = m_lastVisibleRow = -1;
    bool looksLikeCommand = false;

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
                if (!vis) break;
            }
            if (vis) {
                m_lastVisibleRow = i;
                if (firstVisRow < 0)
                    firstVisRow = i;
            }
            m_list->setRowHidden(i, !(vis && ++visible));
        }
        // if the user seems to enter a command, unselect any entries and force reselection
        if (m_list->currentIndex().isValid())
           looksLikeCommand = m_input->text().contains('|') || (m_input->text().trimmed().contains(whitespace) && m_bins->stringList().contains(m_input->text().split(whitespace).first()));
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
    if (looksLikeCommand) {
        m_list->setCurrentIndex(QModelIndex());
    } else if (visible > 0 && (row < 0 || m_list->isRowHidden(row))) {
        m_list->setCurrentIndex(m_list->model()->index(firstVisRow, 0, m_list->rootIndex()));
    } else if (!visible || (visible > 1 && shrink && prevVisible == 1)) {
        m_list->setCurrentIndex(QModelIndex());
    }
    prevVisible = visible;
    if (visible == 1 && !shrink && !needle.isEmpty()) {
        QTimer::singleShot(1, this, &Qiq::insertToken); // needs to be delayed to trigger the textChanged after the actual edit
    }
    adjustGeometry();
}

void Qiq::filterInput() {
    if (m_list->model() == m_applications || m_list->model() == m_external)
        return filter(m_input->text(), Partial);

    QString text = m_input->text();
    const int left = text.lastIndexOf(whitespace, m_input->cursorPosition() - 1) + 1;
    int right = text.indexOf(whitespace, m_input->cursorPosition());
    if (right < 0)
        right = text.length();
    text = text.mid(left, right - left);
//    qDebug() << "filter input" << text;
    if (m_list->model() == m_files) {
        QFileInfo fileInfo(text);
        const QString path = fileInfo.dir().absolutePath();
//        qDebug() << "filesystem" << m_files->rootPath() << path;
        if (path != m_files->rootPath()) {
            m_files->setRootPath(path);
            m_list->setCurrentIndex(QModelIndex());
            QModelIndex newRoot = m_files->index(m_files->rootPath());
            m_list->setRootIndex(newRoot);
            m_files->fetchMore(newRoot);
        }
        text = fileInfo.fileName();
    }
    else if (m_list->model() == m_bins && text.isEmpty()) {
//        qDebug() << "disconnect filter";
        disconnect(m_input, &QLineEdit::textEdited, this, &Qiq::filterInput);
        setCurrentWidget(m_status);
    }
    filter(text, Begin);
}

void Qiq::insertToken() {
    if (m_list->model() == m_applications || m_list->model() == m_external)
        return; // nope. Never.
    QString newToken = m_list->currentIndex().data().toString();
    if (m_list->model() == m_files) {
        QString path = m_files->rootPath();
        if (!path.endsWith("/"))
            path += "/";
        newToken = path + newToken;
    }
    QString text = m_input->text();
    const int left = text.lastIndexOf(whitespace, m_input->cursorPosition() - 1) + 1;
    int right = text.indexOf(whitespace, m_input->cursorPosition());
    if (right < 0)
        right = text.length();
    text.replace(left, right - left, newToken);
    m_input->setText(text);
    m_input->setSelection(right, left + newToken.size() - right);
//    m_input->setCursorPosition(left + newToken.size());
}

void Qiq::printOutput(int exitCode) {
    QProcess *process = qobject_cast<QProcess*>(sender());
    if (!process) {
        qDebug() << "wtf got us here?" << sender();
        return;
    }
    QString output;
    if (exitCode) {
        output = "<h3 style=\"color:red;\">" + process->program() + " " + process->arguments().join(" ") + "</h3><pre style=\"color:red;\">";
//        output = process->program() + " " + process->arguments().join(" ") + "\n==================\n";
        m_disp->setTextColor(QColor(208,23,23));
        QByteArray error = process->readAllStandardError();
        if (!error.isEmpty()) {
            output += QString::fromLocal8Bit(error);
        output += "</pre>";
//            output += "<pre style=\"color:red;\">" + QString::fromLocal8Bit(error) + "</pre>";
        }
    } else {
        m_disp->setTextColor(m_disp->palette().color(m_disp->foregroundRole()));
    }
    QByteArray stdout = process->readAllStandardOutput();
    if (!stdout.isEmpty()) {
        if (process->property("qiq_type").toString() == "math") {
            output += "<h1 align=center>" + QString::fromLocal8Bit(stdout) + "</h1>";
        } else if (stdout.contains("<html>")) {
            output += QString::fromLocal8Bit(stdout);
        } else {
            QProcess aha;
            aha.startCommand("aha -x -n");
            if (aha.waitForStarted(250)) {
                aha.write(stdout);
                aha.closeWriteChannel();
                if (aha.waitForFinished(250))
                    stdout = aha.readAllStandardOutput();
            }
            output += "<pre>" + QString::fromLocal8Bit(stdout) + "</pre>";
        }
    }
    if (!output.isEmpty()) {
        m_disp->setHtml(output);
        m_disp->setMinimumWidth(qMax(m_defaultSize.width(), qMin(800, 12+qCeil(m_disp->document()->size().width()))));
        m_disp->setMinimumHeight(qMin(800, 12+qCeil(m_disp->document()->size().height())));
        if (currentWidget() != m_disp)
            setCurrentWidget(m_disp);
        else
            adjustGeometry();
    }
    process->deleteLater();
}

class DetachableProcess : public QProcess
{
public:
    DetachableProcess(QObject *parent = 0) : QProcess(parent){}
    void detach() {
        setProcessState(QProcess::NotRunning);
    }
};

bool Qiq::runInput() {
    if (m_list->model() == m_external) {
        QModelIndex entry = m_list->currentIndex();
        if (entry.isValid()) {
            bool ret = false;
            QString v = entry.data(AppExec).toString();
            if (v.isEmpty())
                v = entry.data().toString();
            if (m_externCmd == "%clip%") {
                QGuiApplication::clipboard()->setText(v, QClipboard::Clipboard);
                QGuiApplication::clipboard()->setText(v, QClipboard::Selection);
                ret = true;
            } else if (m_externCmd == "%print%") {
                m_externalReply = v;
                ret = true;
            } else {
                ret = QProcess::startDetached(m_externCmd, QStringList() << v);
            }
            if (!m_wasVisble)
                hide();
            return ret;
        }
        return false;
    }
    disconnect(m_input, &QLineEdit::textEdited, this, &Qiq::filterInput);
    if (QFileInfo::exists(m_input->text())) {
        return QProcess::startDetached("xdg-open", QStringList() << m_input->text());
    }

    if (m_list->model() == m_applications) {
        QModelIndex entry = m_list->currentIndex();
        if (entry.isValid()) {
            QString exec = entry.data(AppExec).toString();
            exec.remove(QRegularExpression("%[fFuU]"));
            if (entry.data(AppNeedsTE).toBool()) {
                return QProcess::startDetached("urxvt -e", QStringList() << exec, entry.data(AppPath).toString());
            } else {
                QStringList args = QProcess::splitCommand(exec);
                if (!args.isEmpty())
                    exec = args.takeFirst();
                return QProcess::startDetached(exec, args, entry.data(AppPath).toString());
            }
        }
    }

    QProcess *process = new QProcess(this);

    enum Type { Normal = 0, NoOut, ForceOut, Math };
    Type type = Normal;
    QString command = m_input->text();
    if (command.startsWith("=")) {
        type = Math;
        command.remove(0,1);
    } else if (command.startsWith("?")) {
        type = ForceOut;
        command.remove(0,1);
    } else if (command.startsWith("!")) {
        type = NoOut;
        command.remove(0,1);
    } else if (command.contains('|')) {
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

    QMetaObject::Connection processDoneHandler;
    if (type != NoOut) {
        processDoneHandler = connect(process, &QProcess::finished, this, &Qiq::printOutput);
        process->setChildProcessModifier([] {::setsid(); });
    }
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    if (type != NoOut) {
        QTimer::singleShot(3000, this, [=](){
            if (processDoneHandler) {
                process->closeReadChannel(QProcess::StandardOutput);
                process->closeReadChannel(QProcess::StandardError);
                process->closeWriteChannel();
                disconnect(processDoneHandler);
            }
        });
    }
    bool ret = false;
    if (type != Math) {
        if (type == NoOut) {
            QStringList args = QProcess::splitCommand(command);
            if (!args.isEmpty())
                command = args.takeFirst();
            return QProcess::startDetached(command, args);
        }
        process->startCommand(command);
        ret = process->waitForStarted(250);
    }
    if (!ret) {
        process->startCommand("qalc -f -");
        ret = process->waitForStarted(250);
        if (ret) {
            process->setProperty("qiq_type", "math");
            process->write(command.toLocal8Bit());
            process->closeWriteChannel();
        }
    }
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
    m_wasVisble = isVisible();
    show();
    activateWindow();
    connect(m_input, &QLineEdit::textEdited, this, &Qiq::filterInput, Qt::UniqueConnection);
    if (m_externCmd == "%print%") {
        m_externalReply = QString();
        QElapsedTimer time;
        while (m_externalReply.isNull()) {
            time.start();
            QApplication::processEvents();
            QThread::msleep(33-time.elapsed()); // maitain 30fps but don't live-lock in processEvents()
        }
        return m_externalReply;
    }
    return QString();
}

void Qiq::toggle() {
    if (!isVisible()) {
        show();
        activateWindow();
    } else if (isActiveWindow()) {
        hide();
    } else {
        activateWindow();
    }
}