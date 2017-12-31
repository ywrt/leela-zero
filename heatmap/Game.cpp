/*
    This file is part of Leela Zero.
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

#include <stdio.h>

#include <QUuid>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include "Game.h"

Game::Game(const QString& weights, const QString& opt) :
    QProcess(),
    m_cmdLine("./leelaz"),
    m_timeSettings("time_settings 0 1 0"),
    m_resignation(false),
    m_blackToMove(true),
    m_blackResigned(false),
    m_passes(0),
    m_moveNum(0)
{
#ifdef WIN32
    m_cmdLine.append(".exe");
#endif
    m_cmdLine.append(opt);
    m_cmdLine.append(weights);
    m_fileName = QUuid::createUuid().toRfc4122().toHex();
}

bool Game::checkGameEnd() {
    return (m_resignation ||
            m_passes > 1 ||
            m_moveNum > (19 * 19 * 2));
}

void Game::error(int errnum) {
    QTextStream(stdout) << "*ERROR*: ";
    switch(errnum) {
        case Game::NO_LEELAZ:
            QTextStream(stdout)
                << "No 'leelaz' binary found." << endl;
            break;
        case Game::PROCESS_DIED:
            QTextStream(stdout)
                << "The 'leelaz' process died unexpected." << endl;
            break;
        case Game::WRONG_GTP:
            QTextStream(stdout)
                << "Error in GTP response." << endl;
            break;
        case Game::LAUNCH_FAILURE:
            QTextStream(stdout)
                << "Could not talk to engine after launching." << endl;
            break;
        default:
            QTextStream(stdout)
                << "Unexpected error." << endl;
            break;
    }
}

bool Game::waitReady() {
    while (!canReadLine() && state() == QProcess::Running) {
        waitForReadyRead(-1);
    }
    // somebody crashed
    if (state() != QProcess::Running) {
        return false;
    }
    return true;
}

bool Game::eatNewLine() {
    char readBuffer[4096];
    // Eat double newline from GTP protocol
    if (!waitReady()) {
        error(Game::PROCESS_DIED);
        return false;
    }
    auto readCount = readLine(readBuffer, sizeof(readBuffer));
    if(readCount < 0) {
        error(Game::WRONG_GTP);
        return false;
    }
    return true;
}

std::pair<QString, bool> Game::sendGtpCommand(QString cmd) {

    write(qPrintable(cmd + "\n"));

    //QTextStream(stdout) << "Send: " << cmd << "\n";

    waitForBytesWritten(-1);
    if (!waitReady()) {
        error(Game::PROCESS_DIED);
        return { QString(), false };
    }
    char readBuffer[4096];
    int readCount = readLine(readBuffer, sizeof(readBuffer));
    if (readCount <= 0 || readBuffer[0] != '=') {
        QTextStream(stdout) << "GTP: " << readBuffer << endl;
        error(Game::WRONG_GTP);
        return {QString(), false};
    }
    if (!eatNewLine()) {
        error(Game::PROCESS_DIED);
        return {QString(), false};
    }

    //QTextStream(stdout) << "Recv: " << QString(readBuffer);

    return {QString(&readBuffer[2]), true};
}

void Game::checkVersion(const VersionTuple &min_version) {
    auto ver = sendGtpCommand("version");
    if (!ver.second) {
        // Failed.
        exit(EXIT_FAILURE);
    }

    QString version_buff = ver.first;
    version_buff = version_buff.simplified();
    QStringList version_list = version_buff.split(".");
    if (version_list.size() < 2) {
        QTextStream(stdout)
            << "Unexpected Leela Zero version: " << version_buff << endl;
        exit(EXIT_FAILURE);
    }
    if (version_list[0].toInt() < std::get<0>(min_version)
        || (version_list[0].toInt() == std::get<0>(min_version)
           && version_list[1].toInt() < std::get<1>(min_version))) {
        QTextStream(stdout)
            << "Leela version is too old, saw " << version_buff
            << " but expected "
            << std::get<0>(min_version) << "."
            << std::get<1>(min_version) << "." << endl;
        QTextStream(stdout)
            << "Check https://github.com/gcp/leela-zero for updates." << endl;
        exit(EXIT_FAILURE);
    }
}

bool Game::gameStart(const VersionTuple &min_version) {
    QTextStream(stdout) << m_cmdLine << endl;
    start(m_cmdLine);
    if (!waitForStarted()) {
        error(Game::NO_LEELAZ);
        return false;
    }
    // This either succeeds or we exit immediately, so no need to
    // check any return values.
    checkVersion(min_version);
    QTextStream(stdout) << "Engine has started." << endl;
    sendGtpCommand(m_timeSettings);
    QTextStream(stdout) << "Infinite thinking time set." << endl;
    return true;
}

QString Game::get_state(int playouts) {
  auto board = sendGtpCommand("showboard");
  if (!board.second) return QString();
  auto net = sendGtpCommand("heatmap");
  if (!net.second) return QString();
  auto uct = sendGtpCommand(QString("uctheatmap %1").arg(playouts));
  if (!uct.second) return QString();

  return QString("%1 %2 %3").arg(board.first).arg(net.first).arg(uct.first);
}

bool Game::move() {
    m_moveNum++;
    QString moveCmd;
    if (m_blackToMove) {
        moveCmd = "genmove b";
    } else {
        moveCmd = "genmove w";
    }

    auto res = sendGtpCommand(moveCmd);
    if (!res.second) return false;  // Something went wrong.

    // Skip "= "
    m_moveDone = res.first;
    m_moveDone = m_moveDone.simplified();

    QTextStream(stdout) << m_moveNum << " (";
    QTextStream(stdout) << (m_blackToMove ? "B " : "W ") << m_moveDone << ") ";
    QTextStream(stdout).flush();
    if (m_moveDone.compare(QStringLiteral("pass"),
                          Qt::CaseInsensitive) == 0) {
        m_passes++;
    } else if (m_moveDone.compare(QStringLiteral("resign"),
                                 Qt::CaseInsensitive) == 0) {
        m_resignation = true;
        m_blackResigned = m_blackToMove;
    } else {
        m_passes = 0;
    }
    return true;
}

bool Game::setMove(const QString& m) {
    auto res = sendGtpCommand(m);
    if (!res.second) {
        return false;
    }
    m_moveNum++;
    QStringList moves = m.split(" ");
    if (moves.at(2)
        .compare(QStringLiteral("pass"), Qt::CaseInsensitive) == 0) {
        m_passes++;
    } else if (moves.at(2)
               .compare(QStringLiteral("resign"), Qt::CaseInsensitive) == 0) {
        m_resignation = true;
        m_blackResigned = (moves.at(1).compare(QStringLiteral("black"), Qt::CaseInsensitive) == 0);
    } else {
        m_passes = 0;
    }
    m_blackToMove = !m_blackToMove;
    return true;
}

bool Game::nextMove() {
    if(checkGameEnd()) {
        return false;
    }
    m_blackToMove = !m_blackToMove;
    return true;
}

bool Game::getScore() {
    if(m_resignation) {
        if (m_blackResigned) {
            m_winner = QString(QStringLiteral("white"));
            m_result = "W+Resign ";
            QTextStream(stdout) << "Score: " << m_result << endl;
        } else {
            m_winner = QString(QStringLiteral("black"));
            m_result = "B+Resign ";
            QTextStream(stdout) << "Score: " << m_result << endl;
        }
    } else{
        write("final_score\n");
        waitForBytesWritten(-1);
        if (!waitReady()) {
            error(Game::PROCESS_DIED);
            return false;
        }
        char readBuffer[256];
        readLine(readBuffer, sizeof(readBuffer));
        m_result = readBuffer;
        m_result.remove(0, 2);
        if (readBuffer[2] == 'W') {
            m_winner = QString(QStringLiteral("white"));
        } else if (readBuffer[2] == 'B') {
            m_winner = QString(QStringLiteral("black"));
        }
        if (!eatNewLine()) {
            error(Game::PROCESS_DIED);
            return false;
        }
        QTextStream(stdout) << "Score: " << m_result;
    }
    if (m_winner.isNull()) {
        QTextStream(stdout) << "No winner found" << endl;
        return false;
    }
    QTextStream(stdout) << "Winner: " << m_winner << endl;
    return true;
}

int Game::getWinner() {
    if(m_winner.compare(QStringLiteral("white"), Qt::CaseInsensitive) == 0)
        return Game::WHITE;
    else
        return Game::BLACK;
}

bool Game::writeSgf() {
    QTextStream(stdout) << "Writing " << m_fileName + ".sgf" << endl;
    return sendGtpCommand(qPrintable("printsgf " + m_fileName + ".sgf")).second;
}

bool Game::fixSgf(QString& weightFile, bool resignation) {
    QFile sgfFile(m_fileName + ".sgf");
    if (!sgfFile.open(QIODevice::Text | QIODevice::ReadOnly)) {
        return false;
    }
    QString sgfData = sgfFile.readAll();
    QRegularExpression re("\\[Human\\]");
    QString playerName("[Leela Zero ");
    QRegularExpression le("\\[Leela Zero .* ");
    QRegularExpressionMatch match = le.match(sgfData);
    if (match.hasMatch()) {
        playerName = match.captured(0);
    }
    playerName += weightFile.left(8);
    playerName += "]";
    sgfData.replace(re, playerName);

    if(resignation) {
        QRegularExpression oldResult("RE\\[B\\+.*\\]");
        QString newResult("RE[B+Resign] ");
        sgfData.replace(oldResult, newResult);
        if(!sgfData.contains(newResult, Qt::CaseInsensitive)) {
            QRegularExpression oldwResult("RE\\[W\\+.*\\]");
            sgfData.replace(oldwResult, newResult);
        }
        QRegularExpression lastpass(";W\\[tt\\]\\)");
        QString noPass(")");
        sgfData.replace(lastpass, noPass);
    }

    sgfFile.close();
    if(sgfFile.open(QFile::WriteOnly | QFile::Truncate)) {
        QTextStream out(&sgfFile);
        out << sgfData;
    }
    sgfFile.close();

    return true;
}

bool Game::dumpTraining() {
    QTextStream(stdout) << "Dumping " << m_fileName + ".txt" << endl;
    return sendGtpCommand(
        qPrintable("dump_training " + m_winner + " " + m_fileName + ".txt")).second;
}

bool Game::dumpDebug() {
    QTextStream(stdout) << "Dumping " << m_fileName + ".debug.txt" << endl;
    return sendGtpCommand(
        qPrintable("dump_debug " + m_fileName + ".debug.txt")).second;
}

void Game::gameQuit() {
    write(qPrintable("quit\n"));
    waitForFinished(-1);
}
