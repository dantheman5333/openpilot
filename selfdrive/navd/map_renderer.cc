#include "selfdrive/navd/map_renderer.h"

#include <string>
#include <QApplication>
#include <QBuffer>
#include <QDebug>

#include "common/util.h"
#include "common/timing.h"
#include "selfdrive/ui/qt/maps/map_helpers.h"

const float DEFAULT_ZOOM = 13.5; // Don't go below 13 or features will start to disappear
const int WIDTH = 256;
const int HEIGHT = WIDTH;

const int NUM_VIPC_BUFFERS = 4;

const std::string get_style_path() {
  char *basedir = std::getenv("BASEDIR");
  if (basedir != NULL) {
    return std::string(basedir) + "/selfdrive/navd/style.json";
  } else {
    return "/home/batman/openpilot/selfdrive/navd/style.json";
  }
}

MapRenderer::MapRenderer(const QMapboxGLSettings &settings, bool online) : m_settings(settings) {
  QSurfaceFormat fmt;
  fmt.setRenderableType(QSurfaceFormat::OpenGLES);

  ctx = std::make_unique<QOpenGLContext>();
  ctx->setFormat(fmt);
  ctx->create();
  assert(ctx->isValid());

  surface = std::make_unique<QOffscreenSurface>();
  surface->setFormat(ctx->format());
  surface->create();

  ctx->makeCurrent(surface.get());
  assert(QOpenGLContext::currentContext() == ctx.get());

  gl_functions.reset(ctx->functions());
  gl_functions->initializeOpenGLFunctions();

  QOpenGLFramebufferObjectFormat fbo_format;
  fbo.reset(new QOpenGLFramebufferObject(WIDTH, HEIGHT, fbo_format));

  std::string style = util::read_file(get_style_path());
  m_map.reset(new QMapboxGL(nullptr, m_settings, fbo->size(), 1));
  m_map->setCoordinateZoom(QMapbox::Coordinate(0, 0), DEFAULT_ZOOM);
  m_map->setStyleJson(style.c_str());
  m_map->createRenderer();

  m_map->resize(fbo->size());
  m_map->setFramebufferObject(fbo->handle(), fbo->size());
  gl_functions->glViewport(0, 0, WIDTH, HEIGHT);

  if (online) {
    vipc_server.reset(new VisionIpcServer("navd"));
    vipc_server->create_buffers(VisionStreamType::VISION_STREAM_MAP, NUM_VIPC_BUFFERS, false, WIDTH, HEIGHT);
    vipc_server->start_listener();

    pm.reset(new PubMaster({"navThumbnail"}));
    sm.reset(new SubMaster({"liveLocationKalman", "navRoute"}));

    timer = new QTimer(this);
    QObject::connect(timer, SIGNAL(timeout()), this, SLOT(msgUpdate()));
    timer->start(50);
  }
}

void MapRenderer::msgUpdate() {
  sm->update(0);

  if (sm->updated("liveLocationKalman")) {
    auto location = (*sm)["liveLocationKalman"].getLiveLocationKalman();
    auto pos = location.getPositionGeodetic();
    auto orientation = location.getCalibratedOrientationNED();

    bool localizer_valid = (location.getStatus() == cereal::LiveLocationKalman::Status::VALID) && pos.getValid();
    if (localizer_valid) {
      updatePosition(QMapbox::Coordinate(pos.getValue()[0], pos.getValue()[1]), RAD2DEG(orientation.getValue()[2]));
    }
  }

  if (sm->updated("navRoute")) {
    QList<QGeoCoordinate> route;
    auto coords = (*sm)["navRoute"].getNavRoute().getCoordinates();
    for (auto const &c : coords) {
      route.push_back(QGeoCoordinate(c.getLatitude(), c.getLongitude()));
    }
    updateRoute(route);
  }
}

void MapRenderer::updateZoom(float zoom) {
  if (m_map.isNull()) {
    return;
  }

  m_map->setZoom(zoom);
  update();
}

void MapRenderer::updatePosition(QMapbox::Coordinate position, float bearing) {
  if (m_map.isNull()) {
    return;
  }

  m_map->setCoordinate(position);
  m_map->setBearing(bearing);
  update();
}

bool MapRenderer::loaded() {
  return m_map->isFullyLoaded();
}

void MapRenderer::update() {
  gl_functions->glClear(GL_COLOR_BUFFER_BIT);
  m_map->render();
  gl_functions->glFlush();

  sendVipc();
}

void MapRenderer::sendVipc() {
  if (!vipc_server || !loaded()) {
    return;
  }

  QImage cap = fbo->toImage().convertToFormat(QImage::Format_RGB888, Qt::AutoColor);
  uint64_t ts = nanos_since_boot();
  VisionBuf* buf = vipc_server->get_buffer(VisionStreamType::VISION_STREAM_MAP);
  VisionIpcBufExtra extra = {
    .frame_id = frame_id,
    .timestamp_sof = ts,
    .timestamp_eof = ts,
  };

  assert(cap.sizeInBytes() >= buf->len);
  uint8_t* dst = (uint8_t*)buf->addr;
  uint8_t* src = cap.bits();

  // RGB to greyscale
  memset(dst, 128, buf->len);
  for (int i = 0; i < WIDTH * HEIGHT; i++) {
    dst[i] = src[i * 3];
  }

  vipc_server->send(buf, &extra);

  if (frame_id % 100 == 0) {
    // Write jpeg into buffer
    QByteArray buffer_bytes;
    QBuffer buffer(&buffer_bytes);
    buffer.open(QIODevice::WriteOnly);
    cap.save(&buffer, "JPG", 50);

    kj::Array<capnp::byte> buffer_kj = kj::heapArray<capnp::byte>((const capnp::byte*)buffer_bytes.constData(), buffer_bytes.size());

    // Send thumbnail
    MessageBuilder msg;
    auto thumbnaild = msg.initEvent().initNavThumbnail();
    thumbnaild.setFrameId(frame_id);
    thumbnaild.setTimestampEof(ts);
    thumbnaild.setThumbnail(buffer_kj);
    pm->send("navThumbnail", msg);
  }

  frame_id++;
}

uint8_t* MapRenderer::getImage() {
  QImage cap = fbo->toImage().convertToFormat(QImage::Format_RGB888, Qt::AutoColor);

  uint8_t* src = cap.bits();
  uint8_t* dst = new uint8_t[WIDTH * HEIGHT];

  for (int i = 0; i < WIDTH * HEIGHT; i++) {
    dst[i] = src[i * 3];
  }

  return dst;
}

void MapRenderer::updateRoute(QList<QGeoCoordinate> coordinates) {
  if (m_map.isNull()) return;
  initLayers();

  auto route_points = coordinate_list_to_collection(coordinates);
  QMapbox::Feature feature(QMapbox::Feature::LineStringType, route_points, {}, {});
  QVariantMap navSource;
  navSource["type"] = "geojson";
  navSource["data"] = QVariant::fromValue<QMapbox::Feature>(feature);
  m_map->updateSource("navSource", navSource);
  m_map->setLayoutProperty("navLayer", "visibility", "visible");
}

void MapRenderer::initLayers() {
  if (!m_map->layerExists("navLayer")) {
    QVariantMap nav;
    nav["id"] = "navLayer";
    nav["type"] = "line";
    nav["source"] = "navSource";
    m_map->addLayer(nav, "road-intersection");
    m_map->setPaintProperty("navLayer", "line-color", QColor("grey"));
    m_map->setPaintProperty("navLayer", "line-width", 3);
    m_map->setLayoutProperty("navLayer", "line-cap", "round");
  }
}

MapRenderer::~MapRenderer() {
}

extern "C" {
  MapRenderer* map_renderer_init(char *maps_host = nullptr, char *token = nullptr) {
    char *argv[] = {
      (char*)"navd",
      nullptr
    };
    int argc = 0;
    QApplication *app = new QApplication(argc, argv);
    assert(app);

    QMapboxGLSettings settings;
    settings.setApiBaseUrl(maps_host == nullptr ? MAPS_HOST : maps_host);
    settings.setAccessToken(token == nullptr ? get_mapbox_token() : token);

    return new MapRenderer(settings, false);
  }

  void map_renderer_update_zoom(MapRenderer *inst, float zoom) {
    inst->updateZoom(zoom);
    QApplication::processEvents();
  }

  void map_renderer_update_position(MapRenderer *inst, float lat, float lon, float bearing) {
    inst->updatePosition({lat, lon}, bearing);
    QApplication::processEvents();
  }

  void map_renderer_update_route(MapRenderer *inst, char* polyline) {
    inst->updateRoute(polyline_to_coordinate_list(QString::fromUtf8(polyline)));
  }

  void map_renderer_update(MapRenderer *inst) {
    inst->update();
  }

  void map_renderer_process(MapRenderer *inst) {
    QApplication::processEvents();
  }

  bool map_renderer_loaded(MapRenderer *inst) {
    return inst->loaded();
  }

  uint8_t * map_renderer_get_image(MapRenderer *inst) {
    return inst->getImage();
  }

  void map_renderer_free_image(MapRenderer *inst, uint8_t * buf) {
    delete[] buf;
  }
}
