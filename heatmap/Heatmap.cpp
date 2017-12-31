#include <stdio.h>
#include <cmath>

#include <QPaintEvent> 
#include <QPainter>
#include <QPen>

#include "Heatmap.h"

Heatmap::Heatmap(QWidget* parent) : m_board(361), m_old_board(361), m_net(362), m_uct(362) {
   setWindowTitle(tr("Heatmap"));
   resize(1200,600);
}

void Heatmap::draw_heatmap(QPainter& painter, int cx, int cy, int dx, int dy, const std::vector<float>& map) {
  // Draw heatmap
  for (int y = 18 ; y >= 0; --y) {
    for (int x = 0; x < 19; ++x) {
      int sx = (x + 2) * dx;
      int sy = (y + 3) * dy;
      float score = map[x  + y*19];

      QColor color(255,0,0, 255 * std::sqrt(score));

      painter.setPen(QPen(color, 1, Qt::SolidLine));
      painter.setBrush(QBrush(color, Qt::SolidPattern));
      painter.drawEllipse(QPoint(cx + sx, cy + sy), dx/2, dy/2);
    }
  }

  // Draw lines on board.
  painter.setPen(QPen(Qt::black, 2, Qt::SolidLine));
  painter.setBrush(QBrush(Qt::black, Qt::SolidPattern));
  for (int y = 18 ; y >= 0; --y) {
      int sx0 = (0 + 2) * dx + cx;
      int sx1 = (18 + 2) * dx + cx;
      int sy = (y + 3) * dy + cy;
      painter.drawLine(sx0, sy, sx1, sy);
  }

  for (int x = 0 ; x < 19; ++x) {
      int sx = (x + 2) * dx + cx;
      int sy0 = (0 + 3) * dy + cy;
      int sy1 = (18 + 3) * dy + cy;
      painter.drawLine(sx, sy0, sx, sy1);
  }

  // Draw the stones on the board.
  for (int y = 18 ; y >= 0; --y) {
    for (int x = 0; x < 19; ++x) {
      int sx = (x + 2) * dx + cx;
      int sy = (y + 3) * dy + cy;
      int color = m_board[x  + y*19];
      if (color == 1) {
        painter.setPen(QPen(Qt::black, 1, Qt::SolidLine));
        painter.setBrush(QBrush(Qt::black, Qt::SolidPattern));
        painter.drawEllipse(QPoint(sx, sy), dx/2, dy/2);
      } else if (color == 2) {
        painter.setPen(QPen(Qt::black, 1, Qt::SolidLine));
        painter.setBrush(QBrush(Qt::white, Qt::SolidPattern));
        painter.drawEllipse(QPoint(sx, sy), dx/2, dy/2);
      }

      int prev = m_old_board[x  + y*19];
      if (color != prev) {
        // Draw a circle inside the last played stone.
        painter.setPen(QPen(QColor(128,128,128), 2, Qt::SolidLine));
        painter.setBrush(QBrush());
        painter.drawEllipse(QPoint(sx, sy), dx/3, dy/3);
      }
    }
  }

}

void Heatmap::paintEvent(QPaintEvent *event)
{
    QMutexLocker lock(&m_mutex);

    //create a QPainter and pass a pointer to the device.
    //A paint device can be a QWidget, a QPixmap or a QImage
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
 
    int dx = size().width() / 23 / 2;
    int dy = size().height() / 23;

    // Grey background.
    QColor bg(224, 224, 224);
    painter.setBrush(QBrush(bg, Qt::SolidPattern));
    painter.drawRect(0, 0, size().width(), size().height());

    // Set font size appropriately.
    auto font = painter.font();
    font.setPixelSize(dy);
    painter.setFont(font);

    painter.drawText(size().width() / 8, dy, "Raw network");
    painter.drawText(size().width() * 5 / 8, dy, "UCT Search tree");

    font.setPixelSize(dy * 3 / 4);
    painter.setFont(font);

    painter.drawText(size().width() * 7 / 16, dy * 2 / 3, QString("Move %1").arg(m_move));
    painter.drawText(size().width() * 7 / 16, dy * 4 / 3, QString("Playouts %1").arg(m_playouts));

    draw_heatmap(painter, 0, 0, dx, dy, m_net);
    draw_heatmap(painter, size().width() / 2, 0, dx, dy, m_uct);
}


void Heatmap::update_state(int move, int playouts, const QString& state) {
    auto list = state.split(QRegExp("\\s+"), QString::SkipEmptyParts);
    if (list.size() != 361 + 362 + 362) {
      printf("PROBLEM! %d\n", list.size());
      return;
    }

    {
      QMutexLocker lock(&m_mutex);
      for (int i = 0; i < 361; ++i) {
        if (move != m_move) m_old_board[i] = m_board[i];
        m_board[i] = list[i].toInt();
      }
      
      for (int i = 0; i < 362; ++i) {
        m_net[i] = list[361 + i].toFloat();
      }
      for (int i = 0; i < 362; ++i) {
        m_uct[i] = list[361 + 362 + i].toFloat();
      }
    }

    m_move = move;
    m_playouts = playouts;

    update();
}
