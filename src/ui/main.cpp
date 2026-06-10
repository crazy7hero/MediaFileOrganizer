#include "filecopierwindow.h"
#include <QApplication>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 使用 Fusion 风格确保跨平台一致的 UI
    a.setStyle(QStyleFactory::create("Fusion"));

    // 全局字体微调
    QFont font = a.font();
    font.setPointSize(9);
    a.setFont(font);

    FileCopierWindow w;
    w.show();

    return a.exec();
}
