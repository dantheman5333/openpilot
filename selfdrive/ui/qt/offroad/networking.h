#pragma once

#include <QButtonGroup>
#include <QVBoxLayout>
#include <QWidget>

#include "selfdrive/ui/qt/offroad/wifiManager.h"
#include "selfdrive/ui/qt/widgets/input.h"
#include "selfdrive/ui/qt/widgets/ssh_keys.h"
#include "selfdrive/ui/qt/widgets/toggle.h"

class WifiDetails : public QWidget {
  Q_OBJECT

public:
  explicit WifiDetails(QWidget *parent = 0);

private:
  const Network* network;
  LabelControl* ssid_label;
  LabelControl* security_label;

signals:
  void backPress();

public slots:
  void view(const Network &n);
  void refresh(bool should_update = true);
};

class WifiUI : public QWidget {
  Q_OBJECT

public:
  explicit WifiUI(QWidget *parent = 0, WifiManager* wifi = 0);

private:
  WifiManager *wifi = nullptr;
  QVBoxLayout *list_layout = nullptr;
  QLabel *scanningLabel = nullptr;
  QVBoxLayout* main_layout;
  QPixmap lock;
  QPixmap checkmark;
  QPixmap circled_slash;
  QVector<QPixmap> strengths;

signals:
  void connectToNetwork(const Network &n);
  void viewNetwork(const Network &n);

public slots:
  void refresh();
};

class AdvancedNetworking : public QWidget {
  Q_OBJECT
public:
  explicit AdvancedNetworking(QWidget* parent = 0, WifiManager* wifi = 0);

private:
  LabelControl* ipLabel;
  ToggleControl* tetheringToggle;
  WifiManager* wifi = nullptr;
  Params params;

signals:
  void backPress();

public slots:
  void toggleTethering(bool enabled);
  void refresh();
};

class Networking : public QFrame {
  Q_OBJECT

public:
  explicit Networking(QWidget* parent = 0, bool show_advanced = true);
  WifiManager* wifi = nullptr;

private:
  QStackedLayout* main_layout = nullptr;
  QWidget* wifiScreen = nullptr;
  AdvancedNetworking* an = nullptr;

  WifiUI* wifiWidget;
  WifiDetails* detailsWidget;

protected:
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

public slots:
  void refresh();

private slots:
  void connectToNetwork(const Network &n);
  void viewNetwork(const Network &n);
  void wrongPassword(const QString &ssid);
};
