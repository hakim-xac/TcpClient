#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QJsonArray>
#include <QTreeWidgetItem>
#include <QString>

namespace {

    class DirFirstItem : public QTreeWidgetItem
    {
    public:
        using QTreeWidgetItem::QTreeWidgetItem;

        bool operator<(const QTreeWidgetItem& other) const override
        {
            // 0 - папка, 1 - файл
            const int current_type  = data(0, Qt::UserRole).toInt();
            const int other_type = other.data(0, Qt::UserRole).toInt();

            // сортируем по типам, сначала папки потом файлы
            if (current_type != other_type)
                return current_type < other_type;

            // сортируем по имени
            return text(0).localeAwareCompare(other.text(0)) < 0;
        }
    };

    [[nodiscard]]
    QString
    getCurrentTimeStr()
    {
        return "[" + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "]";
    }

    template<typename ... Args>
    [[nodiscard]]
    QString
    Log(Args&& ... args)
    {
        QString res{};
        res.append(getCurrentTimeStr());
        ((res.append(QString{" "} + std::forward<Args>(args))), ...);

        return res;
    }

    [[nodiscard]] QVector<QString>
    parseResponseFromServer(const QByteArray& response)
    {
        QVector<QString> res;
        auto data_as_str { QString::fromUtf8(response) };

        res.append(data_as_str);
        return res;
    }

    [[nodiscard]] std::optional<QByteArray>
    readPacketSync(QTcpSocket* socket, int timeoutMs = 3000)
    {
        // ждем пока прочитается 4 байла длины json
        while (socket->bytesAvailable() < 4)
        {
            // провисли на таймауте, выходим
            if (!socket->waitForReadyRead(timeoutMs))
                return std::nullopt;
        }

        // получаем длину
        QByteArray lenBuf = socket->read(4);
        if (lenBuf.size() != 4)
            return std::nullopt;

        QDataStream ds(lenBuf);
        ds.setByteOrder(QDataStream::BigEndian);
        quint32 len = 0;
        ds >> len;

        if (len == 0 || len > 10 * 1024 * 1024)
            return std::nullopt;

        // читаем сам json, также за таймаут
        while (socket->bytesAvailable() < static_cast<qint64>(len))
        {
            if (!socket->waitForReadyRead(timeoutMs))
                return std::nullopt;
        }

        return socket->read(len);
    }
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    ui->treeWidget_Files->setColumnCount(2);
    ui->treeWidget_Files->setHeaderLabels({"Имя", "Тип"});
    ui->treeWidget_Files->setAlternatingRowColors(true);
    ui->treeWidget_Files->setSortingEnabled(true);
    ui->treeWidget_Files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->treeWidget_Files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    m_socket = new QTcpSocket(this);

    connect(m_socket, &QTcpSocket::connected, this, [this] {
        QJsonObject obj;
        obj["requiredDirectory"] = ui->lineEdit_RequiredDirectory->text();
        const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);

        QByteArray packet;
        QDataStream ds(&packet, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::BigEndian);
        ds << quint32(json.size()); // отправляем длину сообщения
        packet.append(json); // затем само сообщение

        m_socket->write(packet);
        m_socket->flush();
    });

    connect(m_socket, &QTcpSocket::readyRead, this, [this] {

        while (m_socket->bytesAvailable() >= 4)
        {
            const auto body_opt = readPacketSync(m_socket);
            if (!body_opt)
            {
                showError("Ошибка при чтении ответа сервера!");
                return;
            }

            const QJsonDocument doc { QJsonDocument::fromJson(*body_opt) };
            if (!doc.isObject())
            {
                showError("Некорректный ответ сервера");
                continue;
            }

            const QJsonObject obj { doc.object() };

            if (!obj.value("status").toBool(false))
            {
                showError(obj.value("text_error").toString());
                continue;
            }

            showEntries(obj.value("entries").toArray());
        }
    });

    connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError err) {
        if (err == QAbstractSocket::RemoteHostClosedError)
            return;
        showError("Ошибка соединения: " + m_socket->errorString());
    });

    connect(ui->treeWidget_Files, &QTreeWidget::itemDoubleClicked,
        this, [this](QTreeWidgetItem* item, int) {
            if (!item)
                return;

            const bool isDir = (item->data(0, Qt::UserRole).toInt() == 0);
            if (!isDir)
                return;

            // Формируем новый путь
            QString base = ui->lineEdit_RequiredDirectory->text().trimmed();
            if (base.isEmpty())
                base = "/";

            if (!base.endsWith('/') && !base.endsWith('\\'))
                base += '/';

            const QString newPath = base + item->text(0);

            ui->lineEdit_RequiredDirectory->setText(newPath);

            on_pushButton_RequestFromServer_clicked();
        });
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::showError(const QString& text)
{
    ui->textEdit_Response->setPlainText(Log("Ошибка: " + text));
    ui->stackedWidget->setCurrentWidget(ui->page_Error);
}

void MainWindow::showEntries(const QJsonArray& entries)
{
    ui->treeWidget_Files->clear();

    for (const QJsonValue& v : entries)
    {
        const QJsonObject e = v.toObject();
        const QString name = e.value("name").toString();
        const bool isDir = (e.value("type").toString() == "directory");

        auto* item = new DirFirstItem(ui->treeWidget_Files);
        item->setText(0, name);
        item->setText(1, isDir ? "Папка" : "Файл");
        item->setIcon(0, style()->standardIcon(
                             isDir ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
        item->setData(0, Qt::UserRole, isDir ? 0 : 1);
    }

    ui->treeWidget_Files->sortItems(0, Qt::AscendingOrder);
    ui->stackedWidget->setCurrentWidget(ui->page_Files);
}

void MainWindow::on_pushButton_RequestFromServer_clicked()
{
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();

    m_socket->connectToHost(ui->lineEdit_ServerIPAddress->text(), ui->spinBox_ServerPort->value());
}

