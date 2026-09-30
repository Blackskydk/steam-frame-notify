# OpenVR API findings for Milestones 1 through 4

Reviewed against Valve's public OpenVR SDK **v2.15.6** on 2026-09-29.

Primary sources:

- [OpenVR v2.15.6 release](https://github.com/ValveSoftware/openvr/releases/tag/v2.15.6)
- [Pinned `openvr.h`](https://github.com/ValveSoftware/openvr/blob/v2.15.6/headers/openvr.h)
- [Valve dashboard-overlay documentation](https://github.com/ValveSoftware/openvr/wiki/IVROverlay%3A%3ACreateDashboardOverlay)

## Initialization

- `vr::VR_Init(..., vr::VRApplication_Overlay)` is the supported initialization path for an
  application that only interacts with overlays or the dashboard.
- `VR_IsRuntimeInstalled()` and `VR_GetRuntimePath()` are public diagnostics.
- `IVRSystem::GetRuntimeVersion()` is public and explicitly intended for logging, not feature
  detection.

## Dashboard overlays

- The current interface is `IVROverlay_028`.
- `CreateDashboardOverlay(key, friendlyName, mainHandle, thumbnailHandle)` returns both the main
  dashboard overlay and its thumbnail overlay.
- `SetOverlayRaw(handle, buffer, width, height, bytesPerPixel)` is public and can provide the
  thumbnail without a windowing toolkit or graphics context.
- Overlay mouse-event coordinates use a bottom-left origin. Frame Notify converts their Y value
  to the top-left origin used by its raw history renderer before hit testing.
- `PollNextOverlayEvent` is public. `IVRSystem_026` also exposes
  `PollNextEventWithPoseAndOverlays`, which combines system and owned-overlay event queues.
- The public API provides no method for choosing the dashboard entry's exact ordering. SteamVR
  owns that placement.
- The public documentation says dashboard overlay visibility is controlled by the dashboard
  manager. The application should not attempt to force-show its dashboard overlay.
- **The texture's shape decides the panel's shape.** A panel that scrolls by cropping its texture
  (`SetOverlayTextureBounds`) looked wrong on the Steam Frame whenever the texture was not the
  same shape as the cropped view: the area that answered to the pointer was taller than the
  picture, covered SteamVR's own controls underneath, and reported positions that did not match
  the drawn cards (about 60 px off at the top of a 1280x800 view cropped from a 1280x1024
  texture). Frame Notify therefore always uploads a 2560x1600 texture, the viewport's 16:10
  shape, holding a window of the content at the left, and scrolls inside it with the bounds alone.
  The window is uploaded again (16 MB) only when a scroll would leave it, roughly once per 700
  rows. See `src/ui/scroll_texture.h`.
- **Pointer events are positions in the whole texture, not in the shown crop.** An event's x and y
  are the texture coordinates under the pointer (y from the bottom) multiplied by the overlay's
  mouse scale. On the Steam Frame a 2560x1600 texture with a 1280x800 mouse scale reported exactly
  half of the panel position (pointing at x 950, y 78 gave about 475, 39), and scrolling shifted
  the values too. Frame Notify sets the mouse scale to the texture size (so events are texture
  pixels) and converts with the current crop: panel y = (texture height - event y) - (scroll
  offset - window top).

## Notifications

- `IVRNotifications_002` is present in the current public header.
- `CreateNotification` requires an overlay handle, a user value, a public notification type,
  UTF-8 text, a public style, an optional bitmap, and an output notification ID.
- `RemoveNotification` accepts the notification ID.
- Public notification event names exist: `VREvent_Notification_Shown`,
  `VREvent_Notification_Hidden`, `VREvent_Notification_BeginInteraction`, and
  `VREvent_Notification_Destroyed`.
- The header does **not** specify that third-party notifications are retained in SteamVR Quick
  Access history, how notification activation maps to a dashboard overlay, or whether Steam
  Frame's current runtime surfaces them. Those remain experimental questions for later
  milestones; this repository does not claim an answer yet.
- The `--native-notification` experiment now calls the public API with the dashboard's main overlay
  handle, `EVRNotificationType_Transient`, `EVRNotificationStyle_Application`, and a null bitmap so
  SteamVR may use the overlay's icon. It logs the return code, ID, and related public events.

## Steam Frame result

Tested on SteamVR 2.17.10 on the physical Steam Frame:

- `CreateNotification` returns `VRNotificationError_OK` and emits the public shown, hidden, and
  destroyed events.
- SteamVR displays the third-party transient toast.
- The toast cannot be selected.
- The toast is not retained in SteamVR's notification list.

The application therefore uses the original Option B architecture: native SteamVR toasts for
immediate presentation, plus the application's dashboard overlay for its own history.
