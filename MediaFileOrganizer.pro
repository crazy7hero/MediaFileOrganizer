QT += core gui widgets

TARGET = MediaFileOrganizer

CONFIG += c++17
QMAKE_CXXFLAGS += -std=c++17
DEFINES += NOMINMAX
DEFINES += WIN32_LEAN_AND_MEAN

INCLUDEPATH += src/core src/model src/ui src/thirdparty
DEPENDPATH  += src/core src/model src/ui src/thirdparty

SOURCES += \
    src/core/diskinfo.cpp \
    src/core/filecopierengine.cpp \
    src/core/filescanner.cpp \
    src/core/machineinfo.cpp \
    src/core/mediahelper.cpp \
    src/core/ntfsscanner.cpp \
    src/core/ntfspipeserver.cpp \
    src/model/filelistmodel.cpp \
    src/ui/filecopierwindow.cpp \
    src/ui/main.cpp

HEADERS += \
    src/core/dynamicthreadpool.h \
    src/core/diskinfo.h \
    src/core/filecopierengine.h \
    src/core/filescanner.h \
    src/core/machineinfo.h \
    src/core/mediahelper.h \
    src/core/ntfsscanner.h \
    src/core/ntfspipeserver.h \
    src/model/filelistmodel.h \
    src/model/metafilterproxymodel.h \
    src/thirdparty/MediaInfoDLL.h \
    src/ui/filecopierwindow.h

LIBS += -lwbemuuid -lole32 -loleaut32
