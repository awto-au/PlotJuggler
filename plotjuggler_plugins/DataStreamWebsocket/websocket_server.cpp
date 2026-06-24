/*Wensocket PlotJuggler Plugin license(Faircode, Davide Faconti)

Copyright(C) 2018 Philippe Gauthier - ISIR - UPMC
Copyright(C) 2020 Davide Faconti
Permission is hereby granted to any person obtaining a copy of this software and
associated documentation files(the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and / or sell copies("Use") of the Software, and to permit persons
to whom the Software is furnished to do so. The above copyright notice and this permission
notice shall be included in all copies or substantial portions of the Software. THE
SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#include "websocket_server.h"
#include <QTextStream>
#include <QFile>
#include <QMessageBox>
#include <QDebug>
#include <QSettings>
#include <QDialog>
#include <mutex>
#include <QWebSocket>
#include <QIntValidator>
#include <QMessageBox>
#include <chrono>
#include <QJsonDocument>
#include <QJsonObject>

#include "ui_websocket_server.h"
#include "PlotJuggler/dialog_utils.h"

class WebsocketDialog : public QDialog
{
public:
  WebsocketDialog() : QDialog(nullptr), ui(new Ui::WebSocketDialog)
  {
    ui->setupUi(this);
    ui->lineEditPort->setValidator(new QIntValidator());
    setWindowTitle("WebSocket Server");

    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
  }
  ~WebsocketDialog()
  {
    while (ui->layoutOptions->count() > 0)
    {
      auto item = ui->layoutOptions->takeAt(0);
      item->widget()->setParent(nullptr);
    }
    delete ui;
  }
  Ui::WebSocketDialog* ui;
};

WebsocketServer::WebsocketServer()
  : _running(false), _server("plotJuggler", QWebSocketServer::NonSecureMode)
{
  connect(&_server, &QWebSocketServer::newConnection, this, &WebsocketServer::onNewConnection);
}

WebsocketServer::~WebsocketServer()
{
  shutdown();
}

// start() uses stored QSettings — no dialog. Use gear icon "Configure..." to change port.
bool WebsocketServer::start(QStringList*)
{
  if (_running)
    return _running;

  if (parserFactories() == nullptr || parserFactories()->empty())
  {
    QMessageBox::warning(nullptr, tr("WebSocket Server"), tr("No available MessageParsers"),
                         QMessageBox::Ok);
    _running = false;
    return false;
  }

  QSettings settings;
  QString protocol = settings.value("WebsocketServer::protocol", "JSON").toString();
  if (parserFactories()->find(protocol) == parserFactories()->end())
    protocol = parserFactories()->begin()->first;
  int port = settings.value("WebsocketServer::port", 9871).toInt();

  _parser = parserFactories()->at(protocol)->createParser({}, {}, {}, dataMap());

  if (_server.listen(QHostAddress::Any, port))
  {
    qDebug() << "WebSocket control+data server listening on port" << port;
    _running = true;
  }
  else
  {
    QMessageBox::warning(nullptr, tr("WebSocket Server"),
                         tr("Couldn't open WebSocket on port %1").arg(port), QMessageBox::Ok);
    _running = false;
  }
  return _running;
}

void WebsocketServer::configure()
{
  if (parserFactories() == nullptr || parserFactories()->empty())
    return;

  WebsocketDialog* dialog = new WebsocketDialog();

  for (const auto& it : *parserFactories())
  {
    dialog->ui->comboBoxProtocol->addItem(it.first);
    if (auto widget = it.second->optionsWidget())
    {
      widget->setVisible(false);
      dialog->ui->layoutOptions->addWidget(widget);
    }
  }

  QSettings settings;
  dialog->ui->lineEditPort->setText(
      QString::number(settings.value("WebsocketServer::port", 9871).toInt()));
  dialog->ui->comboBoxProtocol->setCurrentText(
      settings.value("WebsocketServer::protocol", "JSON").toString());

  if (dialog->exec() != QDialog::Accepted)
  {
    dialog->deleteLater();
    return;
  }

  bool ok = false;
  int port = dialog->ui->lineEditPort->text().toUShort(&ok);
  QString protocol = dialog->ui->comboBoxProtocol->currentText();
  dialog->deleteLater();

  settings.setValue("WebsocketServer::port", port);
  settings.setValue("WebsocketServer::protocol", protocol);

  if (_running)
  {
    shutdown();
    start(nullptr);
  }
}

const std::vector<QAction*>& WebsocketServer::availableActions()
{
  if (_actions.empty())
  {
    auto* action = new QAction(tr("Configure WebSocket Server..."), this);
    connect(action, &QAction::triggered, this, &WebsocketServer::configure);
    _actions.push_back(action);
  }
  return _actions;
}

void WebsocketServer::sendToAll(const QString& msg)
{
  for (QWebSocket* client : _clients)
  {
    if (client->isValid())
      client->sendTextMessage(msg);
  }
}

void WebsocketServer::shutdown()
{
  if (_running)
  {
    socketDisconnected();
    _server.close();
    _running = false;
  }
}

void WebsocketServer::onNewConnection()
{
  QWebSocket* pSocket = _server.nextPendingConnection();
  connect(pSocket, &QWebSocket::textMessageReceived, this, &WebsocketServer::processMessage);
  connect(pSocket, &QWebSocket::disconnected, this, &WebsocketServer::socketDisconnected);
  _clients << pSocket;
  qDebug() << "WebSocket client connected:" << pSocket->peerAddress().toString();
}

void WebsocketServer::processMessage(QString message)
{
  QJsonParseError err;
  QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &err);

  if (err.error != QJsonParseError::NoError || !doc.isObject())
  {
    qDebug() << "WebSocket: invalid JSON:" << err.errorString();
    return;
  }

  QJsonObject obj = doc.object();

  // Control message: has a "cmd" key — dispatch to MainWindow, not to parser.
  if (obj.contains("cmd"))
  {
    emit commandReceived(obj);
    return;
  }

  // Data message: forward to parser as before.
  std::lock_guard<std::mutex> lock(mutex());

  using namespace std::chrono;
  auto ts = high_resolution_clock::now().time_since_epoch();
  double timestamp = 1e-6 * double(duration_cast<microseconds>(ts).count());

  QByteArray bmsg = message.toLocal8Bit();
  MessageRef msg(reinterpret_cast<uint8_t*>(bmsg.data()), bmsg.size());

  try
  {
    _parser->parseMessage(msg, timestamp);
  }
  catch (std::exception& err)
  {
    QMessageBox::warning(nullptr, tr("WebSocket Server"),
                         tr("Problem parsing the message. WebSocket Server will be "
                            "stopped.\n%1")
                             .arg(err.what()),
                         QMessageBox::Ok);
    shutdown();
    emit closed();
    return;
  }
  emit dataReceived();
}

void WebsocketServer::socketDisconnected()
{
  QWebSocket* pClient = qobject_cast<QWebSocket*>(sender());
  if (pClient)
  {
    disconnect(pClient, &QWebSocket::textMessageReceived, this, &WebsocketServer::processMessage);
    disconnect(pClient, &QWebSocket::disconnected, this, &WebsocketServer::socketDisconnected);
    _clients.removeAll(pClient);
    pClient->deleteLater();
  }
}
