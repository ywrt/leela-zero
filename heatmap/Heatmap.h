#ifndef HEATMAP_H_
#define HEATMAP_H_

#include <vector>

#include <QWidget>
#include <QMutex>
 
class Heatmap : public QWidget
{
    Q_OBJECT
public:
    Heatmap(QWidget *parent = 0);
 
    void update_state(int move, int playouts, const QString& s);
protected:
    void paintEvent(QPaintEvent *event);
signals:
 
public slots:
 
private:
    void draw_heatmap(QPainter&, int x, int y, int dx, int dy, const std::vector<float>& map);

    std::vector<int> m_board;
    std::vector<int> m_old_board;
    std::vector<float> m_net;
    std::vector<float> m_uct;

    int m_move = 0;
    int m_playouts = 0;

    QMutex m_mutex;
};


#endif
