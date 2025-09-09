#pragma once

#include "audiocollector.hpp"
#include "audioprovider.hpp"
#include <cava/cavacore.h>
#include <qqmlintegration.h>

namespace caelestia {

class CavaProcessor : public AudioProcessor {
    Q_OBJECT

public:
    explicit CavaProcessor(AudioCollector* collector, QObject* parent = nullptr);
    ~CavaProcessor();

signals:
    void valuesChanged(QVector<double> values);

protected:
    void setCollector(AudioCollector* collector) override;

private:
    struct cava_plan* m_plan;
    double* m_in;
    double* m_out;

    int m_bars;
    QVector<double> m_values;

    Q_INVOKABLE void setBars(int bars);

    void reload();
    void initCava();
    void cleanup();

    void process() override;
};

class CavaProvider : public AudioProvider {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int bars READ bars WRITE setBars NOTIFY barsChanged)

    Q_PROPERTY(QVector<double> values READ values NOTIFY valuesChanged)

public:
    explicit CavaProvider(QObject* parent = nullptr);

    [[nodiscard]] int bars() const;
    void setBars(int bars);

    [[nodiscard]] QVector<double> values() const;

signals:
    void barsChanged();
    void valuesChanged();

private:
    int m_bars;
    QVector<double> m_values;

    void updateValues(QVector<double> values);
};

} // namespace caelestia
