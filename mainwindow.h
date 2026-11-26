#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonDocument>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:

    void on_pushButton_RequestFromServer_clicked();

private:
    void showError(const QString& text);
    void showEntries(const QJsonArray& entries);
private:
    Ui::MainWindow *ui;
    QTcpSocket* m_socket{ nullptr };

};
#endif // MAINWINDOW_H
