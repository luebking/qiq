HEADERS = qiq.h gauge.h notifications.h
SOURCES = main.cpp qiq.cpp gauge.cpp notifications.cpp
QT      += dbus gui widgets
#lessThan(QT_MAJOR_VERSION, 6){
#  unix:!macx:QT += x11extras
#}
TARGET  = qiq

#unix:!macx:LIBS    += -lX11
#unix:!macx:DEFINES += WS_X11

# override: qmake PREFIX=/some/where/else
isEmpty(PREFIX) {
  PREFIX = /usr
}

target.path = $$PREFIX/bin

docs.files = doc/*
docs.path = $$PREFIX/share/doc/qiq

INSTALLS += target docs
