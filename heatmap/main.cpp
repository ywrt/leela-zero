/*
    This file is part of Leela Zero.
    Copyright (C) 2017 Gian-Carlo Pascutto
    Copyright (C) 2017 Marco Calignano

    Leela Zero is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leela Zero is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <QtCore/QTimer>
#include <QtCore/QTextStream>
#include <QtCore/QStringList>
#include <QCommandLineParser>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <chrono>
#ifdef WIN32
#include <direct.h>
#endif
#include <QCommandLineParser>
#include <iostream>

#include "Game.h"
#include "Heatmap.h"

#include <QApplication>
#include <QtConcurrent>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("heatmap");
    app.setApplicationVersion(QString("v%1").arg(1));

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();

    // Process the actual command line arguments given by the user
    parser.process(app);

    // Map streams
    QTextStream cin(stdin, QIODevice::ReadOnly);
    QTextStream cout(stdout, QIODevice::WriteOnly);

    QTextStream cerr(stderr, QIODevice::WriteOnly);

#ifdef WIN32
    // We need to make sure these files we need are there before calling them.
    // Otherwise it will result in a crash.
    QFileInfo curl_exe("curl.exe");
    QFileInfo gzip_exe("gzip.exe");
    QFileInfo leelaz_exe("leelaz.exe");
    if (!(curl_exe.exists() && gzip_exe.exists() && leelaz_exe.exists())) {
        char cwd[_MAX_PATH];
        _getcwd(cwd, _MAX_PATH);
        cerr << "Autogtp cannot run as required executables ";
        cerr << "(curl.exe, gzip.exe and leelaz.exe) are not found in the ";
        cerr << "following folder: " << endl;
        cerr << cwd << endl;
        cerr << "Press a key to exit..." << endl;
        getchar();
        return EXIT_FAILURE;
    }
#endif

    Heatmap h;
    h.show();

    QtConcurrent::run([&h, &cout]() -> void {
        Game game("weights.txt", " -t 1 -q -g --noponder -p 50000 -w");
        if (!game.gameStart(VersionTuple{0,8})) return;
        printf("Running!\n");
        do {
            QString state = game.get_state();
            h.update_state(state);
            QThread::sleep(1);

            auto g = h.grab();
            static int num = 0;
            QString filename = QString("move%1.png").arg(num++, 3, 10, QChar('0'));
            if (!g.save(filename, "PNG")) {
              printf("Failed!\n");
            }

            game.move();
        } while (game.nextMove());
    });

    return app.exec();
}
