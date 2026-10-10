// SPDX-License-Identifier: GPL-3.0-or-later
// F-INFO-04: mini map via MKMapSnapshotter (network access by MapKit; requested only after
// the user's click, see MapPreviewController).

#include "MapSnapshot.h"

#include <QCoreApplication>
#include <QPainter>
#include <QPointer>

#include <memory>

#import <AppKit/AppKit.h>
#import <MapKit/MapKit.h>

namespace lf {

namespace {

constexpr double kSpanMeters = 1500.0; // visible area around the position

QImage toQImage(NSImage* image, qreal dpr) {
    CGImageRef cg = [image CGImageForProposedRect:nullptr context:nil hints:nil];
    if (!cg)
        return {};
    const size_t w = CGImageGetWidth(cg), h = CGImageGetHeight(cg);
    QImage out(int(w), int(h), QImage::Format_RGBA8888_Premultiplied);
    if (out.isNull())
        return {};
    out.fill(Qt::transparent);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx =
        CGBitmapContextCreate(out.bits(), w, h, 8, size_t(out.bytesPerLine()), space,
                              uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(space);
    if (!ctx)
        return {};
    CGContextDrawImage(ctx, CGRectMake(0, 0, CGFloat(w), CGFloat(h)), cg);
    CGContextRelease(ctx);
    out.setDevicePixelRatio(dpr);
    return out;
}

class MacMapSnapshotProvider final : public MapSnapshotProvider {
public:
    ~MacMapSnapshotProvider() override { cancel(); }

    void request(const GeoPoint& p, QSize pointSize, qreal dpr, QObject* context, Done done) override {
        cancel();
        MKMapSnapshotOptions* options = [[MKMapSnapshotOptions alloc] init];
        const CLLocationCoordinate2D coord = CLLocationCoordinate2DMake(p.latitude, p.longitude);
        options.region = MKCoordinateRegionMakeWithDistance(coord, kSpanMeters, kSpanMeters);
        options.size = NSMakeSize(pointSize.width(), pointSize.height());
        options.appearance = [NSApp effectiveAppearance];
        MKMapSnapshotter* snapshotter = [[MKMapSnapshotter alloc] initWithOptions:options];
        m_snapshotter = snapshotter;
        QPointer<QObject> guard(context);
        const quint64 token = ++*m_token;
        std::shared_ptr<quint64> current = m_token; // outlives the provider (main thread only)
        auto* self = this;
        [snapshotter startWithQueue:dispatch_get_main_queue()
                  completionHandler:^(MKMapSnapshot* snapshot, NSError* error) {
                    if (!guard || token != *current)
                        return; // replaced, cancelled, or the provider/panel is gone
                    self->m_snapshotter = nil;
                    if (!snapshot) {
                        const QString msg =
                            error ? QString::fromNSString(error.localizedDescription)
                                  : QCoreApplication::translate("lf::MapSnapshot",
                                                                "Die Karte konnte nicht geladen werden.");
                        done(QImage(), msg);
                        return;
                    }
                    QImage img = toQImage(snapshot.image, dpr > 0 ? dpr : 1.0);
                    if (!img.isNull()) { // a pin at the position
                        const NSPoint pt = [snapshot pointForCoordinate:coord];
                        QPainter painter(&img);
                        painter.setRenderHint(QPainter::Antialiasing);
                        painter.setPen(QPen(Qt::white, 2));
                        painter.setBrush(QColor(220, 40, 40));
                        painter.drawEllipse(QPointF(pt.x, pt.y), 6, 6);
                    }
                    done(img, QString());
                  }];
    }

    void cancel() override {
        ++*m_token;
        if (m_snapshotter) {
            [m_snapshotter cancel];
            m_snapshotter = nil;
        }
    }

private:
    MKMapSnapshotter* m_snapshotter = nil;
    std::shared_ptr<quint64> m_token = std::make_shared<quint64>(0);
};

} // namespace

std::unique_ptr<MapSnapshotProvider> createMapSnapshotProvider() {
    return std::make_unique<MacMapSnapshotProvider>();
}

} // namespace lf
