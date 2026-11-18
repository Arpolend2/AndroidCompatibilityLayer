#include "../camera_qt_frame.h"

#include <QAbstractVideoBuffer>

#include <algorithm>
#include <cassert>
#include <vector>

class PlanarBuffer : public QAbstractPlanarVideoBuffer
{
public:
    PlanarBuffer(bool planar, int reportedBytes = 12, bool overlap = false, int yStride = 4,
                 bool separate = false)
        : QAbstractPlanarVideoBuffer(NoHandle), m_planar(planar), m_reportedBytes(reportedBytes),
          m_overlap(overlap), m_yStride(yStride), m_separate(separate)
    {
        m_data[0] = 5;
        m_data[1] = 6;
        m_data[4] = 7;
        m_data[8] = 1;
        m_data[9] = 2;
        m_data[12] = 3;
        m_data[13] = 4;
        m_y = {1, 2, 0, 0, 3, 4, 0, 0};
        m_uv = {5, 6, 0, 0};
    }

    MapMode mapMode() const override { return m_mode; }
    int map(MapMode mode, int *bytes, int strides[4], uchar *data[4]) override
    {
        m_mode = mode;
        *bytes = m_reportedBytes;
        strides[0] = m_yStride;
        data[0] = m_separate ? m_y.data() : m_data + 8;
        strides[1] = m_planar ? 2 : 4;
        data[1] = m_separate ? m_uv.data() : (m_overlap ? m_data + 10 : m_data);
        if (m_planar) {
            strides[2] = 2;
            data[2] = m_data + 4;
        }
        return m_planar ? 3 : 2;
    }
    void unmap() override { m_mode = NotMapped; }

private:
    uchar m_data[16] = {};
    MapMode m_mode = NotMapped;
    bool m_planar;
    int m_reportedBytes;
    bool m_overlap;
    int m_yStride;
    bool m_separate;
    std::vector<uchar> m_y;
    std::vector<uchar> m_uv;
};

int main()
{
    std::vector<uint8_t> pixels;
    QVideoFrame nv12(new PlanarBuffer(false), QSize(2, 2), QVideoFrame::Format_NV12);
    assert(!aurora_camera::copyMappedFrame(nv12, &pixels, 1));
    assert(nv12.map(QAbstractVideoBuffer::ReadOnly));
    assert(nv12.bits(1) < nv12.bits(0));
    assert(aurora_camera::copyMappedFrame(nv12, &pixels, 1));
    assert((pixels == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    nv12.unmap();

    QVideoFrame separate(new PlanarBuffer(false, 12, false, 4, true), QSize(2, 2),
                         QVideoFrame::Format_NV12);
    assert(separate.map(QAbstractVideoBuffer::ReadOnly));
    assert(aurora_camera::copyMappedFrame(separate, &pixels, 1));
    assert((pixels == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    separate.unmap();

    QVideoFrame contiguous(12, QSize(2, 2), 4, QVideoFrame::Format_NV12);
    assert(contiguous.map(QAbstractVideoBuffer::ReadWrite));
    const uchar data[] = {1, 2, 0, 0, 3, 4, 0, 0, 5, 6, 0, 0};
    std::copy(data, data + sizeof(data), contiguous.bits(0));
    assert(aurora_camera::copyMappedFrame(contiguous, &pixels, 1));
    assert((pixels == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    contiguous.unmap();

    QVideoFrame nv21(new PlanarBuffer(false), QSize(2, 2), QVideoFrame::Format_NV21);
    assert(nv21.map(QAbstractVideoBuffer::ReadOnly));
    assert(aurora_camera::copyMappedFrame(nv21, &pixels, 1));
    assert((pixels == std::vector<uint8_t>{1, 2, 3, 4, 6, 5}));
    nv21.unmap();

    QVideoFrame planar(new PlanarBuffer(true), QSize(2, 2), QVideoFrame::Format_YUV420P);
    assert(planar.map(QAbstractVideoBuffer::ReadOnly));
    assert(aurora_camera::copyMappedFrame(planar, &pixels, 1));
    assert((pixels == std::vector<uint8_t>{1, 2, 3, 4, 5, 7}));
    planar.unmap();

    QVideoFrame yv12(new PlanarBuffer(true), QSize(2, 2), QVideoFrame::Format_YV12);
    assert(yv12.map(QAbstractVideoBuffer::ReadOnly));
    assert(aurora_camera::copyMappedFrame(yv12, &pixels, 1));
    assert((pixels == std::vector<uint8_t>{1, 2, 3, 4, 7, 5}));
    yv12.unmap();

    for (QVideoFrame invalid : {
             QVideoFrame(new PlanarBuffer(false, 11), QSize(2, 2), QVideoFrame::Format_NV12),
             QVideoFrame(new PlanarBuffer(false, 12, true), QSize(2, 2), QVideoFrame::Format_NV12),
             QVideoFrame(new PlanarBuffer(false, 12, false, 1), QSize(2, 2), QVideoFrame::Format_NV12)}) {
        assert(invalid.map(QAbstractVideoBuffer::ReadOnly));
        assert(!aurora_camera::copyMappedFrame(invalid, &pixels, 1));
        invalid.unmap();
    }
}
