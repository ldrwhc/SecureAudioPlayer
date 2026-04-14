QT += core gui widgets multimedia network
QT += gui-private

CONFIG += c++17 private_tests resources_big
TEMPLATE = app
TARGET = SecureAudioPlayer

SOURCES += secure_player.cpp

exists(embedded_payload.qrc) {
    RESOURCES += embedded_payload.qrc
}

INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtCore/private
INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtGui/private
INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtWidgets/private
INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtMultimedia/private

win32 {
    LIBS += -lwinmm -lole32 -luuid
    RC_ICONS += app_icon.ico
    QMAKE_CXXFLAGS += -finput-charset=utf-8 -fexec-charset=utf-8
}

QMAKE_CXXFLAGS += -Wno-deprecated-declarations
