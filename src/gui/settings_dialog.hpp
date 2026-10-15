#pragma once
#include <QDialog>
#include "app_settings.hpp"

class QCheckBox;
class QSpinBox;
class QLineEdit;
class QComboBox;

namespace r3dss::gui {

class SettingsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const AppSettings& s, QWidget* parent = nullptr);
    AppSettings settings() const;

private:
    QCheckBox* gpu_;
    QSpinBox *parallel_, *threads_;
    QLineEdit* naming_;
    QComboBox* logLevel_;
    AppSettings base_;
};

} // namespace r3dss::gui
