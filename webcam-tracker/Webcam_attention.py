#!/usr/bin/env python3
"""
Webcam Eye-Tracking Attention Scorer — JARVIS EDITION
=======================================================
Uses MediaPipe Face Mesh (478 landmarks including iris) to compute
real-time attention scores from eye tracking.

JARVIS UI Features:
  - Holographic HUD grid with perspective vanishing point
  - Central "Arc Reactor" circular attention gauge with animated arcs
  - Geometric corner bracket face targeting with snap animation
  - Rotating hexagonal status rings per detected face
  - Neon eye contour wireframes with iris crosshair reticles
  - Gaze direction vector arrows
  - Scan-line sweep animation
  - Glass-morphism data panels with SFSS tracking readouts
  - Dynamic color-coded attention states (Cyan/Amber/Red)

Attention model (UNCHANGED):
  1. Eye Aspect Ratio (EAR) - eye openness (drowsy vs alert)
  2. Iris gaze centering    - looking at screen vs away
  3. Blink rate             - fatigue detection
  4. Gaze stability         - focus vs distraction
  5. Head pose              - looking away or down reduces attention
  6. PERCLOS                - sustained closure over a short window

Output: u8 attention score [0, 255] per detected face,
        POSTed to the webcam-agent for SFSS encryption.

Requirements:
  pip install opencv-python mediapipe requests numpy Pillow

Usage:
  python webcam_attention_jarvis.py                    # default webcam
  python webcam_attention_jarvis.py --camera 1         # specific camera
  python webcam_attention_jarvis.py --max-faces 4      # track up to 4 people
  python webcam_attention_jarvis.py --agent http://127.0.0.1:8083
"""

import argparse
import collections
import math
import sys
import threading
import time

import cv2
import mediapipe as mp
import numpy as np
import requests

# ═══════════════════════════════════════════════════════════════════════════
#  JARVIS UI CONFIGURATION
# ═══════════════════════════════════════════════════════════════════════════

# Core color palette
JARVIS_BLUE    = (255, 229, 0)     # Cyan in BGR: (0, 229, 255)
DEEP_BLUE      = (142, 59, 0)      # Deep blue in BGR: (0, 59, 142)
CYAN_GLOW      = (255, 212, 77)    # Bright cyan in BGR: (77, 212, 255)
NAVY_BG        = (30, 15, 10)      # Dark navy in BGR: (10, 15, 30)
NEON_RED       = (0, 0, 255)       # Red in BGR
AMBER          = (0, 191, 255)     # Amber/Gold in BGR
WHITE          = (255, 255, 255)
DARK_OVERLAY   = (20, 15, 10)      # Semi-transparent dark
GREEN_STATUS   = (100, 255, 100)   # Online green

# UI dimensions (relative to frame)
HUD_MARGIN = 60
GAUGE_RADIUS_RATIO = 0.18
GRID_ROWS = 10
GRID_COLS = 10
SCANLINE_INTERVAL = 5.0  # seconds between scan-line sweeps

# ═══════════════════════════════════════════════════════════════════════════
#  ORIGINAL CODE: MediaPipe Face Mesh landmarks — UNCHANGED
# ═══════════════════════════════════════════════════════════════════════════

# EAR (Eye Aspect Ratio) landmarks - 6 points per eye
RIGHT_EYE_EAR = [33, 160, 158, 133, 153, 144]
LEFT_EYE_EAR = [362, 385, 387, 263, 373, 380]

# Iris center landmarks (MediaPipe refine_landmarks=True)
RIGHT_IRIS_CENTER = 468
LEFT_IRIS_CENTER = 473

# Eye corner landmarks for gaze direction
RIGHT_EYE_INNER = 33
RIGHT_EYE_OUTER = 133
LEFT_EYE_INNER = 362
LEFT_EYE_OUTER = 263

# Upper/lower eyelid for additional openness check
RIGHT_EYE_TOP = 159
RIGHT_EYE_BOT = 145
LEFT_EYE_TOP = 386
LEFT_EYE_BOT = 374

# ═══════════════════════════════════════════════════════════════════════════
#  ORIGINAL CODE: Attention model parameters — UNCHANGED
# ═══════════════════════════════════════════════════════════════════════════

EAR_BLINK_THRESHOLD = 0.21
EAR_CLOSED_THRESHOLD = 0.16
EAR_OPEN_BASELINE = 0.28
BLINK_COOLDOWN_FRAMES = 3

# Scoring weights
W_OPENNESS = 0.28
W_GAZE_CTR = 0.22
W_BLINK = 0.15
W_STABILITY = 0.15
W_HEADPOSE = 0.20

# Gaze stability window
STABILITY_WINDOW = 30

# Blink rate scoring
BLINKS_PER_MIN_NORMAL = 17
BLINKS_PER_MIN_HIGH = 30

# Smoothing
SCORE_SMOOTHING = 0.30

# PERCLOS / closure window
CLOSURE_WINDOW = 90
CLOSED_FRAME_PENALTY = 5


# ═══════════════════════════════════════════════════════════════════════════
#  ORIGINAL CODE: FaceTracker class — UNCHANGED
# ═══════════════════════════════════════════════════════════════════════════

class FaceTracker:
    """Per-face state for attention computation."""

    def __init__(self, face_id: int):
        self.face_id = face_id
        self.registered = False
        self.user_id = None

        self.ear_history = collections.deque(maxlen=120)
        self.blink_count = 0
        self.blink_timestamps = collections.deque(maxlen=60)
        self.frames_since_blink = 999
        self.was_blink = False

        self.gaze_history = collections.deque(maxlen=STABILITY_WINDOW)
        self.closure_history = collections.deque(maxlen=CLOSURE_WINDOW)
        self.closed_frames = 0

        self.smoothed_score = 128.0
        self.last_raw_score = 128

        self.first_seen = time.time()
        self.last_seen = time.time()
        self.frame_count = 0


# ═══════════════════════════════════════════════════════════════════════════
#  ORIGINAL CODE: Math functions — UNCHANGED
# ═══════════════════════════════════════════════════════════════════════════

def distance(p1, p2):
    """Euclidean distance between two 3D landmark points."""
    return math.sqrt((p1.x - p2.x) ** 2 + (p1.y - p2.y) ** 2 + (p1.z - p2.z) ** 2)


def distance_2d(p1, p2):
    """Euclidean distance in x, y only."""
    return math.sqrt((p1.x - p2.x) ** 2 + (p1.y - p2.y) ** 2)


def compute_ear(landmarks, indices):
    """
    Eye Aspect Ratio.
    EAR = (||p2-p6|| + ||p3-p5||) / (2 * ||p1-p4||)
    """
    p1, p2, p3, p4, p5, p6 = [landmarks[i] for i in indices]
    vertical1 = distance(p2, p6)
    vertical2 = distance(p3, p5)
    horizontal = distance(p1, p4)
    if horizontal < 1e-6:
        return 0.0
    return (vertical1 + vertical2) / (2.0 * horizontal)


def compute_gaze_center_ratio(landmarks):
    """
    Compute how centered the iris is within the eye.
    Returns 0.0 to 1.0, where 1.0 means looking straight at camera.
    """
    r_inner = landmarks[RIGHT_EYE_INNER]
    r_outer = landmarks[RIGHT_EYE_OUTER]
    r_iris = landmarks[RIGHT_IRIS_CENTER]
    r_width = distance_2d(r_inner, r_outer)
    if r_width < 1e-6:
        r_ratio = 0.5
    else:
        r_offset = distance_2d(r_inner, r_iris)
        r_ratio = r_offset / r_width

    l_inner = landmarks[LEFT_EYE_INNER]
    l_outer = landmarks[LEFT_EYE_OUTER]
    l_iris = landmarks[LEFT_IRIS_CENTER]
    l_width = distance_2d(l_inner, l_outer)
    if l_width < 1e-6:
        l_ratio = 0.5
    else:
        l_offset = distance_2d(l_inner, l_iris)
        l_ratio = l_offset / l_width

    avg_ratio = (r_ratio + l_ratio) / 2.0

    r_top = landmarks[RIGHT_EYE_TOP]
    r_bot = landmarks[RIGHT_EYE_BOT]
    r_v_height = distance_2d(r_top, r_bot)
    if r_v_height < 1e-6:
        v_ratio = 0.5
    else:
        r_v_offset = distance_2d(r_top, r_iris)
        v_ratio = r_v_offset / r_v_height

    h_center = 1.0 - abs(avg_ratio - 0.5) * 2.0
    v_center = 1.0 - abs(v_ratio - 0.5) * 2.0
    h_center = max(0.0, min(1.0, h_center))
    v_center = max(0.0, min(1.0, v_center))

    return 0.7 * h_center + 0.3 * v_center


def compute_head_pose_score(landmarks, w, h):
    """
    Returns 0.0 to 1.0.
    1.0 means roughly forward-facing.
    0.0 means turned away or looking down too much.
    """
    image_points = np.array([
        (landmarks[1].x * w, landmarks[1].y * h),      # nose tip
        (landmarks[33].x * w, landmarks[33].y * h),     # left eye corner
        (landmarks[263].x * w, landmarks[263].y * h),   # right eye corner
        (landmarks[61].x * w, landmarks[61].y * h),     # left mouth corner
        (landmarks[291].x * w, landmarks[291].y * h),   # right mouth corner
        (landmarks[199].x * w, landmarks[199].y * h),   # chin
    ], dtype=np.float64)

    model_points = np.array([
        (0.0, 0.0, 0.0),
        (-30.0, -30.0, -30.0),
        (30.0, -30.0, -30.0),
        (-25.0, 30.0, -30.0),
        (25.0, 30.0, -30.0),
        (0.0, 60.0, -40.0),
    ], dtype=np.float64)

    focal_length = float(w)
    center = (w / 2.0, h / 2.0)
    camera_matrix = np.array([
        [focal_length, 0, center[0]],
        [0, focal_length, center[1]],
        [0, 0, 1],
    ], dtype=np.float64)

    dist_coeffs = np.zeros((4, 1), dtype=np.float64)

    success, rotation_vector, translation_vector = cv2.solvePnP(
        model_points,
        image_points,
        camera_matrix,
        dist_coeffs,
        flags=cv2.SOLVEPNP_ITERATIVE,
    )

    if not success:
        return 0.5

    rotation_matrix, _ = cv2.Rodrigues(rotation_vector)
    proj_matrix = np.hstack((rotation_matrix, translation_vector))
    _, _, _, _, _, _, euler_angles = cv2.decomposeProjectionMatrix(proj_matrix)

    pitch = abs(float(euler_angles[0][0]))
    yaw = abs(float(euler_angles[1][0]))

    pitch_score = 1.0 - min(pitch / 25.0, 1.0)
    yaw_score = 1.0 - min(yaw / 35.0, 1.0)

    return max(0.0, min(1.0, 0.6 * pitch_score + 0.4 * yaw_score))


def compute_attention(tracker: FaceTracker, landmarks, fps: float, w: int, h: int) -> int:
    """
    Compute attention score [0, 255] from eye landmarks.
    """
    tracker.frame_count += 1
    tracker.last_seen = time.time()

    # 1. Eye openness
    ear_r = compute_ear(landmarks, RIGHT_EYE_EAR)
    ear_l = compute_ear(landmarks, LEFT_EYE_EAR)
    ear = (ear_r + ear_l) / 2.0
    tracker.ear_history.append(ear)

    eyes_closed = ear < EAR_CLOSED_THRESHOLD
    tracker.closure_history.append(1 if eyes_closed else 0)
    if eyes_closed:
        tracker.closed_frames += 1
    else:
        tracker.closed_frames = 0

    perclos = sum(tracker.closure_history) / max(len(tracker.closure_history), 1)

    # Blink detection
    tracker.frames_since_blink += 1
    if ear < EAR_BLINK_THRESHOLD and not tracker.was_blink:
        if tracker.frames_since_blink > BLINK_COOLDOWN_FRAMES:
            tracker.blink_count += 1
            tracker.blink_timestamps.append(time.time())
            tracker.frames_since_blink = 0
        tracker.was_blink = True
    elif ear >= EAR_BLINK_THRESHOLD:
        tracker.was_blink = False

    if eyes_closed:
        openness_score = 0.0
    else:
        openness_score = (ear - EAR_CLOSED_THRESHOLD) / (EAR_OPEN_BASELINE - EAR_CLOSED_THRESHOLD)
        openness_score = max(0.0, min(1.0, openness_score))

    # 2. Gaze centering
    gaze_center = compute_gaze_center_ratio(landmarks)
    if eyes_closed:
        gaze_center = 0.0
    else:
        tracker.gaze_history.append(gaze_center)

    # 3. Blink rate
    now = time.time()
    while tracker.blink_timestamps and (now - tracker.blink_timestamps[0]) > 60:
        tracker.blink_timestamps.popleft()
    blinks_per_min = len(tracker.blink_timestamps)

    if blinks_per_min <= BLINKS_PER_MIN_NORMAL:
        blink_score = 1.0
    elif blinks_per_min >= BLINKS_PER_MIN_HIGH:
        blink_score = 0.2
    else:
        blink_score = 1.0 - 0.8 * (blinks_per_min - BLINKS_PER_MIN_NORMAL) / (
            BLINKS_PER_MIN_HIGH - BLINKS_PER_MIN_NORMAL
        )

    if eyes_closed and tracker.closed_frames > CLOSED_FRAME_PENALTY:
        blink_score = 0.0

    # 4. Gaze stability
    if eyes_closed:
        stability_score = 0.0
    elif len(tracker.gaze_history) >= 5:
        gaze_arr = np.array(list(tracker.gaze_history))
        gaze_variance = np.var(gaze_arr)
        stability_score = 1.0 - min(gaze_variance / 0.03, 1.0)
    else:
        stability_score = 0.5

    # 5. Head pose
    head_pose_score = compute_head_pose_score(landmarks, w, h)

    # 6. Hard suppression for sustained closure or high PERCLOS
    if tracker.closed_frames > CLOSED_FRAME_PENALTY or perclos > 0.6:
        raw = 0.0
    else:
        raw = (
            W_OPENNESS * openness_score +
            W_GAZE_CTR * gaze_center +
            W_BLINK * blink_score +
            W_STABILITY * stability_score +
            W_HEADPOSE * head_pose_score
        )

    raw_score = int(max(0, min(255, raw * 255)))

    # Faster drop when attention falls
    alpha = 0.55 if raw_score < tracker.smoothed_score else SCORE_SMOOTHING
    tracker.smoothed_score = (
        alpha * raw_score +
        (1 - alpha) * tracker.smoothed_score
    )

    final_score = int(round(tracker.smoothed_score))
    final_score = max(0, min(255, final_score))
    tracker.last_raw_score = final_score

    return final_score


# ═══════════════════════════════════════════════════════════════════════════
#  JARVIS UI DRAWING FUNCTIONS (REPLACEMENT)
# ═══════════════════════════════════════════════════════════════════════════

_blink_state = {"phase": 0.0, "direction": 1}
_scanline = {"y": -1, "last_time": 0.0, "active": False}
_global_frame_counter = [0]


def _glow_color(base_color, intensity=1.0):
    """Brighten a BGR color for glow effects."""
    return tuple(min(255, int(c * intensity)) for c in base_color)


def _lerp_color(color1, color2, t):
    """Linear interpolate between two BGR colors."""
    return tuple(int(color1[i] + (color2[i] - color1[i]) * t) for i in range(3))


def _score_to_gradient_color(score):
    """Map attention score 0-255 to a gradient: Red -> Gold -> Cyan."""
    if score <= 120:
        t = score / 120.0
        return _lerp_color(NEON_RED, AMBER, t)
    elif score <= 200:
        t = (score - 120) / 80.0
        return _lerp_color(AMBER, CYAN_GLOW, t)
    else:
        t = (score - 200) / 55.0
        return _lerp_color(CYAN_GLOW, WHITE, min(1.0, t))


def _get_status_label(score):
    """Get attention state label and color."""
    if score > 180:
        return "FOCUSED", CYAN_GLOW
    elif score > 120:
        return "ATTENTIVE", JARVIS_BLUE
    elif score > 60:
        return "DISTRACTED", AMBER
    else:
        return "DROWSY", NEON_RED


def draw_jarvis_grid(frame, w, h, frame_counter):
    """
    Draw a JARVIS-style perspective HUD grid overlay.
    Subtle cyan lines converging toward a vanishing point.
    """
    overlay = np.zeros_like(frame)
    vp_x = w // 2
    vp_y = int(h * 0.35)

def draw_scanline(frame, w, h, current_time):
    """
    Draw an animated horizontal scan-line sweep.
    """
    global _scanline

    if not _scanline["active"]:
        if current_time - _scanline["last_time"] > SCANLINE_INTERVAL:
            _scanline["active"] = True
            _scanline["y"] = 0
            _scanline["last_time"] = current_time
        return

    _scanline["y"] += int(h * 0.04)  # Speed
    y = _scanline["y"]

    if y >= h:
        _scanline["active"] = False
        return

    overlay = np.zeros_like(frame)
    # Main beam
    cv2.line(overlay, (0, y), (w, y), CYAN_GLOW, 3, cv2.LINE_AA)
    # Glow above and below
    cv2.line(overlay, (0, y - 4), (w, y - 4), JARVIS_BLUE, 1, cv2.LINE_AA)
    cv2.line(overlay, (0, y + 4), (w, y + 4), JARVIS_BLUE, 1, cv2.LINE_AA)

    # Highlight intensity fades as it moves
    intensity = 1.0 - (y / h) * 0.5
    cv2.addWeighted(overlay, 0.4 * intensity, frame, 1.0, 0, frame)


def draw_central_gauge(frame, w, h, score, frame_counter):
    """
    Draw the central 'Arc Reactor' circular attention gauge.
    Large circular display at screen center showing the primary attention score.
    """
def draw_hexagon(img, center, radius, color, thickness=1, rotation=0):
    """Draw a regular hexagon."""
    pts = []
    for i in range(6):
        angle = math.radians(60 * i - 90 + rotation)
        x = int(center[0] + radius * math.cos(angle))
        y = int(center[1] + radius * math.sin(angle))
        pts.append((x, y))
    pts_arr = np.array(pts, np.int32).reshape((-1, 1, 2))
    cv2.polylines(img, [pts_arr], True, color, thickness, cv2.LINE_AA)
    


def draw_corner_brackets(frame, x1, y1, x2, y2, color, thickness=2, bracket_len=0.25):
    """
    Draw JARVIS-style L-shaped corner brackets around a bounding box.
    """
    bw = x2 - x1
    bh = y2 - y1
    bl = int(min(bw, bh) * bracket_len)

    # Top-left
    cv2.line(frame, (x1, y1), (x1 + bl, y1), color, thickness, cv2.LINE_AA)
    cv2.line(frame, (x1, y1), (x1, y1 + bl), color, thickness, cv2.LINE_AA)
    # Top-right
    cv2.line(frame, (x2, y1), (x2 - bl, y1), color, thickness, cv2.LINE_AA)
    cv2.line(frame, (x2, y1), (x2, y1 + bl), color, thickness, cv2.LINE_AA)
    # Bottom-left
    cv2.line(frame, (x1, y2), (x1 + bl, y2), color, thickness, cv2.LINE_AA)
    cv2.line(frame, (x1, y2), (x1, y2 - bl), color, thickness, cv2.LINE_AA)
    # Bottom-right
    cv2.line(frame, (x2, y2), (x2 - bl, y2), color, thickness, cv2.LINE_AA)
    cv2.line(frame, (x2, y2), (x2, y2 - bl), color, thickness, cv2.LINE_AA)


def draw_glowing_line(frame, p1, p2, color, thickness=2, glow_intensity=0.4):
    """Draw a line with a glow effect by drawing multiple faded layers."""
    # Outer glow
    glow = tuple(int(c * glow_intensity) for c in color)
    cv2.line(frame, p1, p2, glow, thickness + 3, cv2.LINE_AA)
    cv2.line(frame, p1, p2, glow, thickness + 1, cv2.LINE_AA)
    # Core line
    cv2.line(frame, p1, p2, color, thickness, cv2.LINE_AA)


def draw_iris_reticle(frame, cx, cy, color, frame_counter):
    """
    Draw a 4-point crosshair reticle on the iris center.
    Animated with subtle pulsing.
    """
    pulse = int(3 + 2 * math.sin(frame_counter * 0.15))
    # Crosshair
    cv2.line(frame, (cx - 10, cy), (cx - 3, cy), color, 1, cv2.LINE_AA)
    cv2.line(frame, (cx + 3, cy), (cx + 10, cy), color, 1, cv2.LINE_AA)
    cv2.line(frame, (cx, cy - 10), (cx, cy - 3), color, 1, cv2.LINE_AA)
    cv2.line(frame, (cx, cy + 3), (cx, cy + 10), color, 1, cv2.LINE_AA)
    # Center dot
    cv2.circle(frame, (cx, cy), 2, color, -1, cv2.LINE_AA)
    # Outer ring (pulsing)
    cv2.circle(frame, (cx, cy), 6 + pulse, color, 1, cv2.LINE_AA)


def draw_gaze_vector(frame, start_x, start_y, gaze_center, color):
    """
    Draw a gaze direction vector as a dotted line.
    """
    dx = int((gaze_center - 0.5) * 80)
    dy = int((0.5 - gaze_center) * 30)  # Slight vertical component
    end_x = start_x + dx
    end_y = start_y + dy

    # Dotted line effect
    dist = math.sqrt(dx ** 2 + dy ** 2)
    if dist > 0:
        steps = int(dist / 5)
        for i in range(0, steps, 2):
            t1 = i / steps
            t2 = min(1.0, (i + 1) / steps)
            p1 = (int(start_x + (end_x - start_x) * t1), int(start_y + (end_y - start_y) * t1))
            p2 = (int(start_x + (end_x - start_x) * t2), int(start_y + (end_y - start_y) * t2))
            cv2.line(frame, p1, p2, color, 1, cv2.LINE_AA)

    # Arrowhead
    cv2.circle(frame, (end_x, end_y), 3, color, -1, cv2.LINE_AA)


def draw_data_ribbon(frame, x_base, y_base, tracker, score, ear, gaze_stability):
    """
    Draw a vertical data ribbon next to a face bracket.
    Shows technical readouts for the tracked face.
    """
    status, status_color = _get_status_label(score)
    lines = [
        (f"FACE [{tracker.face_id}]", JARVIS_BLUE),
        (f"EAR: {ear:.2f}", WHITE),
        (f"GZE: {gaze_stability:.2f}", WHITE),
        (f"ATT: {score}/255", status_color),
        (status, status_color),
    ]

    if tracker.registered:
        lines.append((f"SFSS: OK", GREEN_STATUS))
    else:
        lines.append((f"SFSS: ...", AMBER))

    # Semi-transparent background panel
    panel_w = 140
    panel_h = len(lines) * 18 + 10
    overlay = frame.copy()
    cv2.rectangle(overlay, (x_base, y_base), (x_base + panel_w, y_base + panel_h),
                  NAVY_BG, -1)
    cv2.addWeighted(overlay, 0.75, frame, 0.25, 0, frame)

    # Border
    cv2.rectangle(frame, (x_base, y_base), (x_base + panel_w, y_base + panel_h),
                  JARVIS_BLUE, 1, cv2.LINE_AA)

    # Corner accents
    cl = 8
    cv2.line(frame, (x_base, y_base), (x_base + cl, y_base), CYAN_GLOW, 1, cv2.LINE_AA)
    cv2.line(frame, (x_base, y_base), (x_base, y_base + cl), CYAN_GLOW, 1, cv2.LINE_AA)
    cv2.line(frame, (x_base + panel_w, y_base + panel_h),
             (x_base + panel_w - cl, y_base + panel_h), CYAN_GLOW, 1, cv2.LINE_AA)
    cv2.line(frame, (x_base + panel_w, y_base + panel_h),
             (x_base + panel_w, y_base + panel_h - cl), CYAN_GLOW, 1, cv2.LINE_AA)

    for i, (text, color) in enumerate(lines):
        y = y_base + 18 + i * 16
        cv2.putText(frame, text, (x_base + 8, y),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.4, color, 1, cv2.LINE_AA)


def draw_overlay(frame, landmarks, tracker, score, w, h):
    """
    JARVIS-style eye tracking visualization overlay.
    Draws futuristic HUD elements: corner brackets, hexagonal rings,
    neon eye contours, iris reticles, gaze vectors, and data ribbons.
    """
    _global_frame_counter[0] += 1
    fc = _global_frame_counter[0]

    # Face bounding box
    xs = [int(l.x * w) for l in landmarks]
    ys = [int(l.y * h) for l in landmarks]
    x1, x2 = min(xs), max(xs)
    y1, y2 = min(ys), max(ys)

    # Expand box slightly for aesthetics
    pad = 15
    x1 = max(0, x1 - pad)
    y1 = max(0, y1 - pad)
    x2 = min(w, x2 + pad)
    y2 = min(h, y2 + pad)

    score_color = _score_to_gradient_color(score)
    status, status_color = _get_status_label(score)

    # ── Corner brackets ──
    draw_corner_brackets(frame, x1, y1, x2, y2, score_color, thickness=2, bracket_len=0.22)

    # ── Hexagonal status ring (rotating) ──
    hex_cx = (x1 + x2) // 2
    hex_cy = (y1 + y2) // 2
    hex_r = max(x2 - x1, y2 - y1) // 2 + 10
    rotation = int((fc * 1.5) % 360)  # Slow rotation
    draw_hexagon(frame, (hex_cx, hex_cy), hex_r, _glow_color(score_color, 0.4),
                 thickness=1, rotation=rotation)
    draw_hexagon(frame, (hex_cx, hex_cy), hex_r - 5, _glow_color(score_color, 0.25),
                 thickness=1, rotation=-rotation // 2)

    # ── Eye contours (neon wireframe) ──
    for indices, color in [(RIGHT_EYE_EAR, JARVIS_BLUE), (LEFT_EYE_EAR, CYAN_GLOW)]:
        pts = [(int(landmarks[i].x * w), int(landmarks[i].y * h)) for i in indices]
        for i in range(len(pts)):
            draw_glowing_line(frame, pts[i], pts[(i + 1) % len(pts)], color, thickness=1)

    # ── Iris centers with reticles ──
    for iris_idx, color in [(RIGHT_IRIS_CENTER, JARVIS_BLUE), (LEFT_IRIS_CENTER, CYAN_GLOW)]:
        cx = int(landmarks[iris_idx].x * w)
        cy = int(landmarks[iris_idx].y * h)
        draw_iris_reticle(frame, cx, cy, color, fc)

    # ── Gaze direction vector ──
    r_eye = landmarks[RIGHT_IRIS_CENTER]
    rx = int(r_eye.x * w)
    ry = int(r_eye.y * h)
    gaze_center = tracker.gaze_history[-1] if tracker.gaze_history else 0.5
    draw_gaze_vector(frame, rx, ry, gaze_center, _glow_color(JARVIS_BLUE, 0.6))

    # ── Data ribbon (right side of face) ──
    ear_r = compute_ear(landmarks, RIGHT_EYE_EAR)
    ear_l = compute_ear(landmarks, LEFT_EYE_EAR)
    ear = (ear_r + ear_l) / 2.0
    gaze_stab = 0.5
    if len(tracker.gaze_history) >= 5:
        gaze_stab = 1.0 - min(np.var(np.array(list(tracker.gaze_history))) / 0.03, 1.0)

    ribbon_x = min(x2 + 10, w - 150)
    draw_data_ribbon(frame, ribbon_x, y1, tracker, score, ear, gaze_stab)


def draw_jarvis_hud(frame, w, h, fps, active_faces, trackers, frame_counter):
    """
    Draw the global JARVIS HUD overlay:
    - Perspective grid
    - Central attention gauge (uses highest score)
    - Top-left system clock
    - Bottom-left SFSS tracking panel
    - Top-right status indicators
    - Scan-line sweep
    """
    current_time = time.time()

    # 1. Perspective grid
    draw_jarvis_grid(frame, w, h, frame_counter)

    # 2. Scan-line sweep
    draw_scanline(frame, w, h, current_time)

    # 3. Central gauge (show highest attention score among all faces)
    best_score = 0
    if trackers:
        best_score = max(t.last_raw_score for t in trackers.values())
    draw_central_gauge(frame, w, h, best_score, frame_counter)

    # 4. Top-left: System clock
    time_str = time.strftime("%H:%M:%S %p")
    cv2.putText(frame, time_str, (HUD_MARGIN, HUD_MARGIN),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, JARVIS_BLUE, 1, cv2.LINE_AA)
    # Decorative line under clock
    cv2.line(frame, (HUD_MARGIN, HUD_MARGIN + 8),
             (HUD_MARGIN + 100, HUD_MARGIN + 8), JARVIS_BLUE, 1, cv2.LINE_AA)

    # 5. Top-right: Status indicators
    # FPS
    cv2.putText(frame, f"FPS: {fps:.0f}", (w - HUD_MARGIN - 80, HUD_MARGIN),
                cv2.FONT_HERSHEY_SIMPLEX, 0.5, WHITE, 1, cv2.LINE_AA)
    # System online with blinking dot
    blink = (frame_counter // 15) % 2 == 0
    dot_color = CYAN_GLOW if blink else (40, 35, 30)
    cv2.circle(frame, (w - HUD_MARGIN - 100, HUD_MARGIN - 5), 4, dot_color, -1, cv2.LINE_AA)
    cv2.putText(frame, "SYSTEM: ONLINE", (w - HUD_MARGIN - 180, HUD_MARGIN),
                cv2.FONT_HERSHEY_SIMPLEX, 0.45, JARVIS_BLUE, 1, cv2.LINE_AA)

    # 6. Bottom-left: SFSS Tracking Panel
    panel_x = HUD_MARGIN
    panel_y = h - HUD_MARGIN - 30

    # Panel background
    overlay = frame.copy()
    panel_h = 30 + len(trackers) * 22 + 15
    cv2.rectangle(overlay, (panel_x - 10, panel_y - 25),
                  (panel_x + 300, panel_y + panel_h), NAVY_BG, -1)
    cv2.addWeighted(overlay, 0.7, frame, 0.3, 0, frame)

    # Panel border with corner accents
    cv2.rectangle(frame, (panel_x - 10, panel_y - 25),
                  (panel_x + 300, panel_y + panel_h), JARVIS_BLUE, 1, cv2.LINE_AA)
    cl = 10
    cv2.line(frame, (panel_x - 10, panel_y - 25),
             (panel_x - 10 + cl, panel_y - 25), CYAN_GLOW, 2, cv2.LINE_AA)
    cv2.line(frame, (panel_x - 10, panel_y - 25),
             (panel_x - 10, panel_y - 25 + cl), CYAN_GLOW, 2, cv2.LINE_AA)

    # Header
    cv2.putText(frame, "SFSS EYE-TRACKING // MULTI-FACE", (panel_x, panel_y),
                cv2.FONT_HERSHEY_SIMPLEX, 0.5, CYAN_GLOW, 1, cv2.LINE_AA)
    cv2.line(frame, (panel_x, panel_y + 5),
             (panel_x + 260, panel_y + 5), JARVIS_BLUE, 1, cv2.LINE_AA)

    # Face rows
    y_off = panel_y + 28
    for fid, trk in sorted(trackers.items()):
        status, status_color = _get_status_label(trk.last_raw_score)
        sfss_status = "SFSS" if trk.registered else "..."
        sfss_color = GREEN_STATUS if trk.registered else AMBER

        # Face ID and score
        cv2.putText(frame, f"Face {fid}", (panel_x, y_off),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.4, WHITE, 1, cv2.LINE_AA)
        cv2.putText(frame, f"{trk.last_raw_score:03d}/255", (panel_x + 55, y_off),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.4, status_color, 1, cv2.LINE_AA)
        # Status pill
        cv2.putText(frame, f"[{status}]", (panel_x + 130, y_off),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.35, status_color, 1, cv2.LINE_AA)
        # SFSS indicator
        cv2.putText(frame, f"[{sfss_status}]", (panel_x + 215, y_off),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.35, sfss_color, 1, cv2.LINE_AA)

        y_off += 22

    # Face count summary
    cv2.putText(frame, f"Active: {active_faces}  Tracked: {len(trackers)}",
                (panel_x, y_off + 5),
                cv2.FONT_HERSHEY_SIMPLEX, 0.4, _glow_color(JARVIS_BLUE, 0.7), 1, cv2.LINE_AA)

    # 7. Bottom-right: Decorative branding
    brand_text = "J.A.R.V.I.S. // ATTENTION ANALYTICS"
    tw = cv2.getTextSize(brand_text, cv2.FONT_HERSHEY_SIMPLEX, 0.4, 1)[0][0]
    cv2.putText(frame, brand_text, (w - HUD_MARGIN - tw, h - 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.4, _glow_color(JARVIS_BLUE, 0.5), 1, cv2.LINE_AA)

    # 8. Red alert border for critically low attention
    if best_score < 60:
        alert_pulse = abs(math.sin(frame_counter * 0.1))
        overlay = np.zeros_like(frame)
        cv2.rectangle(overlay, (0, 0), (w, h), NEON_RED, 6)
        cv2.addWeighted(overlay, 0.15 * alert_pulse, frame, 1.0, 0, frame)


# ═══════════════════════════════════════════════════════════════════════════
#  ORIGINAL CODE: WebcamAttentionTracker class — UNCHANGED LOGIC
# ═══════════════════════════════════════════════════════════════════════════

class WebcamAttentionTracker:
    """Main tracker: webcam -> MediaPipe -> attention scores -> SFSS agent."""

    def __init__(self, camera_id=0, max_faces=4, agent_url="http://127.0.0.1:8083",
                 api_key="dev-key-change-in-production", tick_hz=1.0):
        self.camera_id = camera_id
        self.max_faces = max_faces
        self.agent_url = agent_url.rstrip("/")
        self.api_key = api_key
        self.tick_interval = 1.0 / tick_hz

        self.trackers: dict[int, FaceTracker] = {}
        self.face_mesh = mp.solutions.face_mesh.FaceMesh(
            max_num_faces=max_faces,
            refine_landmarks=True,
            min_detection_confidence=0.5,
            min_tracking_confidence=0.5,
        )

        self.fps = 30.0
        self.frame_count = 0
        self.last_tick_time = 0.0
        self.running = True

    def register_face(self, tracker: FaceTracker):
        """Register a new face with the webcam-agent for SFSS session."""
        try:
            resp = requests.post(
                f"{self.agent_url}/face/add",
                json={"face_id": tracker.face_id},
                headers={"Authorization": f"Bearer {self.api_key}"},
                timeout=2,
            )
            if resp.ok:
                data = resp.json()
                tracker.user_id = data.get("user_id")
                tracker.registered = True
                print(f"[TRACKER] Face {tracker.face_id} -> uid:{tracker.user_id} registered with SFSS")
            else:
                print(f"[TRACKER] Failed to register face {tracker.face_id}: HTTP {resp.status_code}")
        except Exception as e:
            print(f"[TRACKER] Agent unreachable for face {tracker.face_id}: {e}")

    def send_score(self, tracker: FaceTracker, score: int):
        """Send attention score to webcam-agent for SFSS encryption."""
        if not tracker.registered:
            return
        try:
            requests.post(
                f"{self.agent_url}/score",
                json={"user_id": tracker.user_id, "score": score},
                headers={"Authorization": f"Bearer {self.api_key}"},
                timeout=1,
            )
        except Exception:
            pass

    def remove_face(self, face_id: int):
        """Remove a face that has not been seen recently."""
        tracker = self.trackers.get(face_id)
        if tracker and tracker.registered:
            try:
                requests.post(
                    f"{self.agent_url}/face/remove",
                    json={"user_id": tracker.user_id},
                    headers={
                        "Authorization": f"Bearer {self.api_key}",
                        "Content-Type": "application/json",
                    },
                    timeout=2,
                )
                print(f"[TRACKER] Face {face_id} (uid:{tracker.user_id}) removed from SFSS")
            except Exception:
                pass
        if face_id in self.trackers:
            del self.trackers[face_id]

    def run(self):
        """Main loop: capture -> detect -> score -> encrypt -> display."""
        cap = cv2.VideoCapture(self.camera_id)
        if not cap.isOpened():
            print(f"[ERROR] Cannot open camera {self.camera_id}")
            sys.exit(1)

        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)

        print(f"[TRACKER] Camera {self.camera_id} opened")
        print(f"[TRACKER] Max faces: {self.max_faces}")
        print(f"[TRACKER] Agent: {self.agent_url}")
        print(f"[TRACKER] Tick rate: {1.0 / self.tick_interval:.0f} Hz")
        print(f"[TRACKER] JARVIS UI active")
        print(f"[TRACKER] Press 'q' to quit")
        print()

        prev_time = time.time()

        while self.running:
            ret, frame = cap.read()
            if not ret:
                print("[ERROR] Frame capture failed")
                break

            self.frame_count += 1
            now = time.time()
            dt = now - prev_time
            if dt > 0:
                self.fps = 0.9 * self.fps + 0.1 * (1.0 / dt)
            prev_time = now

            h, w, _ = frame.shape

            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            results = self.face_mesh.process(rgb)

            active_face_ids = set()
            faces = []

            if results.multi_face_landmarks:
                faces = list(results.multi_face_landmarks)
                faces.sort(key=lambda f: f.landmark[1].x)

            for face_idx, face_lm in enumerate(faces):
                landmarks = face_lm.landmark

                face_id = face_idx
                active_face_ids.add(face_id)

                if face_id not in self.trackers:
                    self.trackers[face_id] = FaceTracker(face_id)
                    threading.Thread(
                        target=self.register_face,
                        args=(self.trackers[face_id],),
                        daemon=True,
                    ).start()

                tracker = self.trackers[face_id]

                score = compute_attention(tracker, landmarks, self.fps, w, h)
                draw_overlay(frame, landmarks, tracker, score, w, h)

                if now - self.last_tick_time >= self.tick_interval:
                    threading.Thread(
                        target=self.send_score,
                        args=(tracker, score),
                        daemon=True,
                    ).start()

            if now - self.last_tick_time >= self.tick_interval:
                self.last_tick_time = now

            stale = [
                fid for fid, t in self.trackers.items()
                if fid not in active_face_ids and (now - t.last_seen) > 5.0
            ]
            for fid in stale:
                threading.Thread(target=self.remove_face, args=(fid,), daemon=True).start()

            # ═══════════════════════════════════════════════════════════
            #  JARVIS HUD OVERLAY (replaces the old basic HUD)
            # ═══════════════════════════════════════════════════════════
            draw_jarvis_hud(frame, w, h, self.fps, len(active_face_ids),
                           self.trackers, self.frame_count)

            cv2.imshow("JARVIS // Eye-Tracking Attention", frame)

            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                break

        print("\n[TRACKER] Shutting down...")
        for fid in list(self.trackers.keys()):
            self.remove_face(fid)
        cap.release()
        cv2.destroyAllWindows()
        self.face_mesh.close()
        print("[TRACKER] Done.")


# ═══════════════════════════════════════════════════════════════════════════
#  ORIGINAL CODE: main() — UNCHANGED
# ═══════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="Webcam Eye-Tracking -> SFSS Attention (JARVIS UI)")
    parser.add_argument("--camera", type=int, default=0, help="Camera device ID")
    parser.add_argument("--max-faces", type=int, default=4, help="Max faces to track")
    parser.add_argument("--agent", type=str, default="http://127.0.0.1:8083",
                        help="Webcam-agent URL")
    parser.add_argument("--api-key", type=str, default="dev-key-change-in-production")
    parser.add_argument("--tick-hz", type=float, default=1.0,
                        help="SFSS tick rate in Hz (scores sent per second)")
    args = parser.parse_args()

    tracker = WebcamAttentionTracker(
        camera_id=args.camera,
        max_faces=args.max_faces,
        agent_url=args.agent,
        api_key=args.api_key,
        tick_hz=args.tick_hz,
    )
    tracker.run()


if __name__ == "__main__":
    main()