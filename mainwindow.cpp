#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QJsonArray>
#include <QTreeWidgetItem>
#include <QString>

namespace {

    // Вспомагательный класс, для нужного отображения данных на форме.
    // Требование: сначало сортировка по типам, папки выше файлов, затем по имени.
    class DirFirstItem : public QTreeWidgetItem
    {
    public:
        // подключаем все конструкторы базового класса. Нужно для ADL
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

    // Формирует строку с стекущим временем.
    [[nodiscard]] QString getCurrentTimeStr()
    {
        return "[" + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "]";
    }

    // Формирует строку из всех переданных агрументов и добавляем вначало текущее время
    template<typename ... Args>
    [[nodiscard]] QString Log(Args&& ... args)
    {
        QString res{};
        res.append(getCurrentTimeStr());
        ((res.append(QString{" "} + std::forward<Args>(args))), ...);

        return res;
    }

    //
    [[nodiscard]] std::optional<QByteArray>
    readPacketSync(QTcpSocket* socket, int timeout_ms = 3000)
    {
        // ждем пока прочитается 4 байла длины json
        while (socket->bytesAvailable() < 4)
        {
            // провисли на таймауте, выходим
            if (!socket->waitForReadyRead(timeout_ms))
                return std::nullopt;
        }

        // получаем длину
        QByteArray len_buf { socket->read(4) };
        if (len_buf.size() != 4)
            return std::nullopt;

        QDataStream ds(len_buf);
        ds.setByteOrder(QDataStream::BigEndian);
        quint32 len = 0;
        ds >> len;

        if (len == 0 || len > 10 * 1024 * 1024)
            return std::nullopt;

        // читаем сам json, также за таймаут
        // ждем пока в буфере не будет нужное количество данных
        while (socket->bytesAvailable() < static_cast<qint64>(len))
        {
            if (!socket->waitForReadyRead(timeout_ms))
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

    // Протокол передачи:
    // 4 байта - длина тела
    // N байт - тело. Тип тела: json строка.
    connect(m_socket, &QTcpSocket::connected, this, [this] {
        QJsonObject obj;
        obj["requiredDirectory"] = ui->lineEdit_RequiredDirectory->text();
        const QByteArray json { QJsonDocument(obj).toJson(QJsonDocument::Compact) };

        QByteArray packet;
        QDataStream ds(&packet, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::BigEndian);
        ds << quint32(json.size()); // отправляем длину сообщения
        packet.append(json); // затем само сообщение

        m_socket->write(packet);
        m_socket->flush();
    });

    // Протокол примема:
    // 4 байта - длина тела
    // N байт - тело. Тип тела: json строка.
    connect(m_socket, &QTcpSocket::readyRead, this, [this] {

        // ждем пока в буфере не будет хотябы 4 байта.
        while (m_socket->bytesAvailable() >= 4)
        {
            const auto body_opt { readPacketSync(m_socket) };
            if (!body_opt) [[unlikely]]
            {
                showError("Ошибка при чтении ответа сервера!");
                return;
            }

            const QJsonDocument doc { QJsonDocument::fromJson(*body_opt) };
            if (!doc.isObject()) [[unlikely]]
            {
                showError("Некорректный ответ сервера");
                continue;
            }

            const QJsonObject obj { doc.object() };

            if (!obj.value("status").toBool(false)) [[unlikely]]
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

    // обработка двойного нажатия на элемент компонента treeWidget
    // если нажание на элемент с типом "каталог", то формируем новый запрос к серверу для получения нового списка.
    connect(ui->treeWidget_Files, &QTreeWidget::itemDoubleClicked,
        this, [this](QTreeWidgetItem* item, int) {
            if (!item)
                return;

            const bool is_dir { item->data(0, Qt::UserRole).toInt() == 0 };
            if (!is_dir)
                return;

            // Формируем новый путь
            QString base { ui->lineEdit_RequiredDirectory->text().trimmed() };
            if (base.isEmpty())
                base = "/";

            if (!base.endsWith('/') && !base.endsWith('\\'))
                base += '/';

            const QString new_path { base + item->text(0) };

            ui->lineEdit_RequiredDirectory->setText(new_path);

            on_pushButton_RequestFromServer_clicked();
        });
}

MainWindow::~MainWindow()
{
    delete ui;
}

// Вызовается в случае ошибки
void MainWindow::showError(const QString& text)
{
    ui->textEdit_Response->setPlainText(Log("Ошибка: " + text));
    ui->stackedWidget->setCurrentWidget(ui->page_Error);
}

// Вызывается в случае, если пришли корректные данные
void MainWindow::showEntries(const QJsonArray& entries)
{
    // очищаем компонент
    ui->treeWidget_Files->clear();

    // заполняем новыми данными
    for (const QJsonValue& v : entries)
    {
        const QJsonObject e { v.toObject() };
        const QString name { e.value("name").toString() };
        const bool is_dir { e.value("type").toString() == "directory" };

        // Устанавливаем свойства в зависимости от типа файла
        // применяем пользовательский тип DirFirstItem, где учитывается правильная сортировка
        auto* item = new DirFirstItem(ui->treeWidget_Files);
        item->setText(0, name);
        item->setText(1, is_dir ? "Папка" : "Файл");
        item->setIcon(0, style()->standardIcon(
                             is_dir ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
        item->setData(0, Qt::UserRole, is_dir ? 0 : 1);
    }

    // сортируем
    ui->treeWidget_Files->sortItems(0, Qt::AscendingOrder);
    // показываем виджет, с компонентом treeWidget_Files
    ui->stackedWidget->setCurrentWidget(ui->page_Files);
}

void MainWindow::on_pushButton_RequestFromServer_clicked()
{
    // закрываем старое событие, если оно было и не дожидаемся отправки тех данных, если они оставались..
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();

    // вызываем событие отправки данных серверу
    m_socket->connectToHost(ui->lineEdit_ServerIPAddress->text(), ui->spinBox_ServerPort->value());
}

