// SPDX-License-Identifier: GPL-3.0-or-later
// F-INFO-08: MKMapView for the folder map (see FolderMapView.h). MapKit loads map tiles from
// Apple's servers; the view exists only while the user has the map open (SPEC 10.3).

#include "FolderMapView.h"

#include <QHash>
#include <QLayout>
#include <QPointer>
#include <QWidget>
#include <QWindow>

#include <algorithm>

#import <AppKit/AppKit.h>
#import <MapKit/MapKit.h>

namespace lf {
class MacMapView;
}

// ---- Objective-C parts ---------------------------------------------------------------------------

@interface LFPhotoAnnotation : NSObject <MKAnnotation>
@property(nonatomic, readwrite) CLLocationCoordinate2D coordinate;
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* path;
@property(nonatomic) BOOL isCurrent;
@end

@implementation LFPhotoAnnotation
@end

// Marker with hover tracking (MapKit has no hover callback on macOS).
@interface LFMarkerView : MKMarkerAnnotationView
@property(nonatomic, copy) void (^hover)(BOOL inside);
@end

@implementation LFMarkerView {
    NSTrackingArea* _area;
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_area)
        [self removeTrackingArea:_area];
    _area = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInActiveApp | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_area];
}
- (void)mouseEntered:(NSEvent*)event {
    [super mouseEntered:event];
    if (self.hover)
        self.hover(YES);
}
- (void)mouseExited:(NSEvent*)event {
    [super mouseExited:event];
    if (self.hover)
        self.hover(NO);
}
- (void)prepareForReuse {
    [super prepareForReuse];
    if (self.hover)
        self.hover(NO);
    self.hover = nil;
}
@end

@interface LFMapDelegate : NSObject <MKMapViewDelegate>
@property(nonatomic, assign) lf::MacMapView* owner; // cleared before the owner goes
@end

namespace lf {

namespace {

NSString* const kPhotoReuse = @"lf.photo";
NSString* const kClusterId = @"lf.photos";

void style(LFMarkerView* v, LFPhotoAnnotation* a) {
    v.clusteringIdentifier = a.isCurrent ? nil : kClusterId; // the current image never clusters
    v.markerTintColor = a.isCurrent ? NSColor.systemRedColor : NSColor.systemBlueColor;
    v.displayPriority = a.isCurrent ? MKFeatureDisplayPriorityRequired : MKFeatureDisplayPriorityDefaultHigh;
    v.zPriority = a.isCurrent ? MKAnnotationViewZPriorityMax : MKAnnotationViewZPriorityDefaultUnselected;
    v.glyphImage = [NSImage imageWithSystemSymbolName:@"photo" accessibilityDescription:nil];
    v.titleVisibility = MKFeatureVisibilityHidden;
    v.canShowCallout = NO;
    v.toolTip = a.title;
}

NSImage* toNSImage(const QImage& image) {
    if (image.isNull())
        return nil;
    CGImageRef cg = image.toCGImage();
    if (!cg)
        return nil;
    const qreal dpr = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
    NSImage* ns = [[NSImage alloc] initWithCGImage:cg
                                              size:NSMakeSize(image.width() / dpr, image.height() / dpr)];
    CGImageRelease(cg);
    return ns;
}

} // namespace

class MacMapView final : public MapViewBackend {
public:
    explicit MacMapView(QWidget* host) : m_host(host) { m_annotations = [NSMutableDictionary dictionary]; }
    ~MacMapView() override { stop(); }

    void start() override {
        if (m_view || !m_host || !m_host->layout())
            return;
        m_view = [[MKMapView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
        m_delegate = [[LFMapDelegate alloc] init];
        m_delegate.owner = this;
        m_view.delegate = m_delegate;
        m_view.showsZoomControls = YES;
        m_view.showsCompass = YES;
        m_view.showsScale = YES;
        m_view.pitchEnabled = NO;
        [m_view registerClass:[LFMarkerView class] forAnnotationViewWithReuseIdentifier:kPhotoReuse];
        // ARC allows an object pointer to become an integer only through a __bridge void*.
        m_window = QWindow::fromWinId(WId((__bridge void*)m_view)); // NOLINT(bugprone-casting-through-void)
        m_container = QWidget::createWindowContainer(m_window, m_host);
        m_container->setObjectName(QStringLiteral("lfFolderMapContainer"));
        m_container->setMinimumSize(160, 120);
        m_host->layout()->addWidget(m_container);
        if (m_annotations.count > 0)
            [m_view addAnnotations:m_annotations.allValues];
    }

    void stop() override {
        hidePreview();
        if (!m_view)
            return;
        m_delegate.owner = nullptr;
        m_view.delegate = nil;
        [m_view removeAnnotations:m_view.annotations];
        [m_annotations removeAllObjects];
        m_current = nil;
        delete m_container.data(); // owns the foreign QWindow around the NSView
        m_container = nullptr;
        m_window = nullptr;
        [m_view removeFromSuperview];
        m_view = nil;
        m_delegate = nil;
        m_programmatic = 0;
    }

    bool isStarted() const override { return m_view != nil; }

    void setMarkers(const QVector<MapMarker>& markers) override {
        if (m_view && m_annotations.count > 0)
            [m_view removeAnnotations:m_annotations.allValues];
        [m_annotations removeAllObjects];
        m_current = nil;
        addMarkers(markers);
    }

    void addMarkers(const QVector<MapMarker>& markers) override {
        NSMutableArray* added = [NSMutableArray arrayWithCapacity:NSUInteger(markers.size())];
        for (const MapMarker& m : markers) {
            NSString* key = m.path.toNSString();
            if (m_annotations[key])
                continue;
            LFPhotoAnnotation* a = [[LFPhotoAnnotation alloc] init];
            a.coordinate = CLLocationCoordinate2DMake(m.point.latitude, m.point.longitude);
            a.title = m.name.toNSString();
            a.path = key;
            a.isCurrent = m.path == m_currentPath;
            if (a.isCurrent)
                m_current = a;
            m_annotations[key] = a;
            [added addObject:a];
        }
        if (m_view && added.count > 0)
            [m_view addAnnotations:added];
    }

    void setCurrent(const QString& path) override {
        m_currentPath = path;
        LFPhotoAnnotation* next = path.isEmpty() ? nil : m_annotations[path.toNSString()];
        if (next == m_current)
            return;
        NSMutableArray* changed = [NSMutableArray array];
        if (m_current) {
            m_current.isCurrent = NO;
            [changed addObject:m_current];
        }
        if (next) {
            next.isCurrent = YES;
            [changed addObject:next];
        }
        m_current = next;
        // Re-adding restyles the markers and lets MapKit re-cluster them.
        if (m_view && changed.count > 0) {
            [m_view removeAnnotations:changed];
            [m_view addAnnotations:changed];
        }
    }

    void showRegion(const GeoRegion& r, bool animated) override {
        if (!m_view)
            return;
        MKCoordinateRegion region = MKCoordinateRegionMake(
            CLLocationCoordinate2DMake(std::clamp(r.center.latitude, -85.0, 85.0), r.center.longitude),
            MKCoordinateSpanMake(std::clamp(r.latitudeDelta, 0.0005, 170.0),
                                 std::clamp(r.longitudeDelta, 0.0005, 360.0)));
        ++m_programmatic; // regionDidChange that follows is ours, not the user's
        [m_view setRegion:[m_view regionThatFits:region] animated:animated ? YES : NO];
    }

    std::optional<GeoRegion> visibleRegion() const override {
        if (!m_view || NSIsEmptyRect(m_view.bounds))
            return std::nullopt;
        const MKCoordinateRegion r = m_view.region;
        return GeoRegion{
            {r.center.latitude, r.center.longitude}, r.span.latitudeDelta, r.span.longitudeDelta};
    }

    void setThumbnail(const QString& path, const QImage& image) override {
        if (!m_view || path != m_hoverPath || !m_hoverView)
            return;
        NSImage* ns = toNSImage(image);
        if (!ns)
            return;
        if (!m_preview) {
            m_preview = [[NSImageView alloc] initWithFrame:NSZeroRect];
            m_preview.imageScaling = NSImageScaleProportionallyUpOrDown;
            m_preview.wantsLayer = YES;
            m_preview.layer.cornerRadius = 6;
            m_preview.layer.masksToBounds = YES;
            m_preview.layer.borderWidth = 2;
            m_preview.layer.borderColor = NSColor.whiteColor.CGColor;
        }
        m_preview.image = ns;
        NSSize size = ns.size;
        const CGFloat maxEdge = 160;
        const CGFloat scale =
            std::min<CGFloat>(1.0, maxEdge / std::max<CGFloat>(1, std::max(size.width, size.height)));
        size =
            NSMakeSize(std::max<CGFloat>(24, size.width * scale), std::max<CGFloat>(24, size.height * scale));
        const NSRect marker = [m_hoverView convertRect:m_hoverView.bounds toView:m_view];
        NSRect frame = NSMakeRect(NSMidX(marker) - size.width / 2, 0, size.width, size.height);
        frame.origin.y = m_view.isFlipped ? NSMinY(marker) - size.height - 4 : NSMaxY(marker) + 4;
        // Keep it inside the map.
        frame.origin.x = std::clamp<CGFloat>(frame.origin.x, 4,
                                             std::max<CGFloat>(4, NSWidth(m_view.bounds) - size.width - 4));
        frame.origin.y = std::clamp<CGFloat>(frame.origin.y, 4,
                                             std::max<CGFloat>(4, NSHeight(m_view.bounds) - size.height - 4));
        m_preview.frame = frame;
        if (m_preview.superview != m_view)
            [m_view addSubview:m_preview];
        m_preview.hidden = NO;
    }

    // ---- called by the delegate / marker views -----------------------------------------------

    void activated(NSString* path) {
        hidePreview();
        if (markerActivated)
            markerActivated(QString::fromNSString(path));
    }
    void hovered(NSString* path, MKAnnotationView* view) {
        if (!path) {
            hidePreview();
            m_hoverPath.clear();
            m_hoverView = nil;
            if (markerHovered)
                markerHovered(QString());
            return;
        }
        m_hoverPath = QString::fromNSString(path);
        m_hoverView = view;
        if (markerHovered)
            markerHovered(m_hoverPath);
    }
    void regionChanged() {
        hidePreview();
        if (m_programmatic > 0) {
            --m_programmatic;
            return;
        }
        if (userMovedMap)
            userMovedMap();
    }
    void zoomedIntoCluster() {
        if (userMovedMap)
            userMovedMap();
    }

private:
    void hidePreview() {
        if (m_preview)
            m_preview.hidden = YES;
    }

    QPointer<QWidget> m_host;
    QPointer<QWidget> m_container;
    QWindow* m_window = nullptr;
    MKMapView* m_view = nil;
    LFMapDelegate* m_delegate = nil;
    NSMutableDictionary<NSString*, LFPhotoAnnotation*>* m_annotations = nil;
    LFPhotoAnnotation* m_current = nil;
    QString m_currentPath;
    NSImageView* m_preview = nil;
    QString m_hoverPath;
    __weak MKAnnotationView* m_hoverView = nil;
    int m_programmatic = 0;
};

std::unique_ptr<MapViewBackend> createMapViewBackend(QWidget* host) {
    return std::make_unique<MacMapView>(host);
}

} // namespace lf

@implementation LFMapDelegate

- (MKAnnotationView*)mapView:(MKMapView*)mapView viewForAnnotation:(id<MKAnnotation>)annotation {
    if ([annotation isKindOfClass:[MKClusterAnnotation class]]) {
        auto* cluster = static_cast<MKClusterAnnotation*>(annotation);
        auto* v = static_cast<MKMarkerAnnotationView*>([mapView
            dequeueReusableAnnotationViewWithIdentifier:MKMapViewDefaultClusterAnnotationViewReuseIdentifier
                                          forAnnotation:annotation]);
        if ([v isKindOfClass:[MKMarkerAnnotationView class]]) {
            v.markerTintColor = NSColor.systemBlueColor;
            v.toolTip = [NSString stringWithFormat:@"%lu", (unsigned long)cluster.memberAnnotations.count];
        }
        return v;
    }
    if (![annotation isKindOfClass:[LFPhotoAnnotation class]])
        return nil;
    auto* a = static_cast<LFPhotoAnnotation*>(annotation);
    auto* v = static_cast<LFMarkerView*>([mapView dequeueReusableAnnotationViewWithIdentifier:lf::kPhotoReuse
                                                                                forAnnotation:annotation]);
    lf::style(v, a);
    __weak LFMapDelegate* weakSelf = self;
    __weak LFMarkerView* weakView = v;
    NSString* path = a.path;
    v.hover = ^(BOOL inside) {
      LFMapDelegate* strongSelf = weakSelf;
      if (strongSelf && strongSelf.owner)
          strongSelf.owner->hovered(inside ? path : nil, weakView);
    };
    return v;
}

- (void)mapView:(MKMapView*)mapView didSelectAnnotationView:(MKAnnotationView*)view {
    id<MKAnnotation> annotation = view.annotation;
    __weak LFMapDelegate* weakSelf = self;
    // Leave MapKit's selection handling before the collection (and so the markers) change.
    dispatch_async(dispatch_get_main_queue(), ^{
      LFMapDelegate* strongSelf = weakSelf;
      if (!strongSelf || !strongSelf.owner)
          return;
      [mapView deselectAnnotation:annotation animated:NO];
      if ([annotation isKindOfClass:[MKClusterAnnotation class]]) {
          [mapView showAnnotations:static_cast<MKClusterAnnotation*>(annotation).memberAnnotations
                          animated:YES];
          strongSelf.owner->zoomedIntoCluster();
      } else if ([annotation isKindOfClass:[LFPhotoAnnotation class]]) {
          strongSelf.owner->activated(static_cast<LFPhotoAnnotation*>(annotation).path);
      }
    });
}

- (void)mapView:(MKMapView*)mapView regionDidChangeAnimated:(BOOL)animated {
    Q_UNUSED(mapView);
    Q_UNUSED(animated);
    if (self.owner)
        self.owner->regionChanged();
}

@end
