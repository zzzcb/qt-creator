// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0+ OR GPL-3.0 WITH Qt-GPL-exception-1.0

#include "pydapengine.h"

#include "dapclient.h"

#include <coreplugin/messagemanager.h>
#include <coreplugin/icore.h>

#include <debugger/debuggermainwindow.h>
#include <debugger/debuggertr.h>

#include <utils/async.h>
#include <utils/infobar.h>
#include <utils/temporarydirectory.h>

#include <projectexplorer/buildconfiguration.h>
#include <projectexplorer/buildsystem.h>
#include <projectexplorer/projecttree.h>

#include <QDebug>
#include <QLoggingCategory>
#include <QTcpSocket>
#include <QTimer>
#include <QVersionNumber>

using namespace Core;
using namespace Utils;

static Q_LOGGING_CATEGORY(dapEngineLog, "qtc.dbg.dapengine", QtWarningMsg)

namespace Debugger::Internal {


const char installDebugPyInfoBarId[] = "Python::InstallDebugPy";

static bool missingPySideInstallation(const FilePath &pythonPath, const QString &packageName)
{
    QTC_ASSERT(!packageName.isEmpty(), return false);
    static QMap<FilePath, QSet<QString>> pythonWithPyside;
    if (pythonWithPyside[pythonPath].contains(packageName))
        return false;

    Process pythonProcess;
    pythonProcess.setCommand({pythonPath, {"-c", "import " + packageName}});
    pythonProcess.runBlocking();
    const bool missing = pythonProcess.result() != ProcessResult::FinishedWithSuccess;
    if (!missing)
        pythonWithPyside[pythonPath].insert(packageName);
    return missing;
}

class TcpSocketDataProvider : public IDataProvider
{
public:
    TcpSocketDataProvider(const DebuggerRunParameters &rp,
                          const CommandLine &cmd,
                          const QString &hostName,
                          const quint16 port,
                          QObject *parent = nullptr)
        : IDataProvider(parent)
        , m_runParameters(rp)
        , m_cmd(cmd)
        , m_hostName(hostName)
        , m_port(port)
    {
        connect(&m_socket, &QTcpSocket::connected, this, &IDataProvider::started);
        connect(&m_socket, &QTcpSocket::disconnected, this, &IDataProvider::done);
        connect(&m_socket, &QTcpSocket::readyRead, this, &IDataProvider::readyReadStandardOutput);
        connect(&m_socket,
                &QTcpSocket::errorOccurred,
                this,
                &IDataProvider::readyReadStandardError);
    }

    ~TcpSocketDataProvider() { m_socket.disconnect(); }

    void start() override
    {
        m_proc.setEnvironment(m_runParameters.debugger.environment);
        m_proc.setCommand(m_cmd);
        m_proc.start();

        m_timer = new QTimer(this);
        m_timer->setInterval(100);

        connect(m_timer, &QTimer::timeout, this, [this]() {
            m_socket.connectToHost(m_hostName, m_port);
            m_socket.waitForConnected();
            if (m_socket.state() == QTcpSocket::ConnectedState)
                m_timer->stop();

            if (m_attempts >= m_maxAttempts)
                kill();

            m_attempts++;
        });

        m_timer->start();
    }

    bool isRunning() const override { return m_socket.isOpen(); }
    void writeRaw(const QByteArray &data) override
    {
        if (m_socket.isOpen())
            m_socket.write(data);
    }
    void kill() override
    {
        m_timer->stop();

        if (m_proc.state() == QProcess::Running)
            m_proc.kill();

        if (m_socket.isOpen())
            m_socket.disconnect();

        m_socket.abort();
        emit done();
    }
    QByteArray readAllStandardOutput() override { return m_socket.readAll(); }
    QString readAllStandardError() override { return QString(); }
    int exitCode() const override { return 0; }
    QString executable() const override { return m_hostName + ":" + QString::number(m_port); }

    QProcess::ExitStatus exitStatus() const override { return QProcess::NormalExit; }
    QProcess::ProcessError error() const override { return QProcess::UnknownError; }
    Utils::ProcessResult result() const override { return ProcessResult::FinishedWithSuccess; }
    QString exitMessage() const override { return QString(); }

private:
    Utils::Process m_proc;
    const DebuggerRunParameters m_runParameters;
    const CommandLine m_cmd;

    QTcpSocket m_socket;
    const QString m_hostName;
    const quint16 m_port;

    QTimer *m_timer;
    const int m_maxAttempts = 10;
    int m_attempts = 0;
};

class PythonDapClient : public DapClient
{
public:
    PythonDapClient(IDataProvider *provider, QObject *parent = nullptr)
        : DapClient(provider, parent)
    {}

    void sendInitialize()
    {
        postRequest("initialize",
                    QJsonObject{{"clientID", "QtCreator"},
                                {"clientName", "QtCreator"},
                                {"adapterID", "python"},
                                {"pathFormat", "path"}});
    }
};

PyDapEngine::PyDapEngine()
    : DapEngine()
{
    setObjectName("PythonDapEngine");
    setDebuggerName("PythonDAP");
    setDebuggerType("DAP");
}

void PyDapEngine::handleDapInitialize()
{
    QTC_ASSERT(state() == EngineRunRequested, qCDebug(dapEngineLog) << state());

    m_dapClient->sendAttach();

    qCDebug(dapEngineLog) << "handleDapAttach";
}

void PyDapEngine::quitDebugger()
{
    showMessage(QString("QUIT DEBUGGER REQUESTED IN STATE %1").arg(state()));
    startDying();

    // Temporary workaround for Python debugging instability, particularly
    // in conjunction with PySide6, due to unreliable pause functionality.
    if (state() == InferiorRunOk) {
        setState(InferiorStopRequested);
        notifyInferiorStopOk();
        return;
    }

    DebuggerEngine::quitDebugger();
}

void installDebugpyPackage(const FilePath &pythonPath)
{
    CommandLine cmd{pythonPath, {"-m", "pip", "install", "debugpy"}};
    Process process;
    process.setCommand(cmd);

    QObject::connect(&process, &Process::textOnStandardError, [](const QString &text) {
        MessageManager::writeSilently("Python debugger: debugpy package installation failed" + text);
    });

    QObject::connect(&process, &Process::done, []() {
        MessageManager::writeSilently("Python debugger: debugpy package installed.");
    });

    process.start();
    process.waitForFinished();
}

void PyDapEngine::setupEngine()
{
    QTC_ASSERT(state() == EngineSetupRequested, qDebug() << state());

    Utils::FilePath interpreter = runParameters().interpreter;

    const FilePath scriptFile = runParameters().mainScript;
    if (!scriptFile.isReadableFile()) {
        MessageManager::writeDisrupting(
            "Python Error" + QString("Cannot open script file %1").arg(scriptFile.toUserOutput()));
        notifyEngineSetupFailed();
        return;
    }

    if (missingPySideInstallation(interpreter, "debugpy")) {
        Utils::InfoBarEntry
            info(installDebugPyInfoBarId,
                 Tr::tr(
                     "Python Debugging Support is not available. Please install debugpy package."),
                 Utils::InfoBarEntry::GlobalSuppression::Enabled);
        info.addCustomButton(Tr::tr("Install debugpy"), [this] {
            Core::ICore::infoBar()->removeInfo(installDebugPyInfoBarId);
            Core::ICore::infoBar()->globallySuppressInfo(installDebugPyInfoBarId);
            QTimer::singleShot(0, this, [interpreter = runParameters().interpreter] {
                Utils::asyncRun(&installDebugpyPackage, interpreter);
            });
        });
        Core::ICore::infoBar()->addInfo(info);

        notifyEngineSetupFailed();
        return;
    }

    CommandLine cmd{interpreter,
                    {"-Xfrozen_modules=off",
                     "-m", "debugpy",
                     "--listen", "127.0.0.1:5679",
                     "--wait-for-client",
                     scriptFile.path(),
                     runParameters().inferior.workingDirectory.path()}};

    IDataProvider *dataProvider
        = new TcpSocketDataProvider(runParameters(), cmd, "127.0.0.1", 5679, this);
    m_dapClient = new PythonDapClient(dataProvider, this);

    connectDataGeneratorSignals();
    m_dapClient->dataProvider()->start();
}

} // namespace Debugger::Internal
