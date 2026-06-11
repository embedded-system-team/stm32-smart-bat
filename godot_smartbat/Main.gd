extends Node3D

# Smart Bat 3D 打擊遊戲 (Godot 4.6)
#
# 整個場景都是在 _ready() 用程式碼建出來的,所以你不用在編輯器裡手動拉節點,
# 只要把這支腳本掛在一個 Node3D 上(main.tscn 已經幫你掛好了)即可。
#
# 揮棒事件由 swing_bridge.py 透過 UDP 4242 傳進來。
#
# 操作:  等 PITCH! 出現後揮棒    SPACE = 下一球    ESC = 離開

const UDP_PORT := 4242

const BALL_START_Z := -20.0   # 投手丘(遠處)
const HIT_Z := 0.0            # 好球帶(打者前方)
const BALL_SPEED := 14.0      # units / 秒,球往 +z(朝鏡頭)飛
const GOOD_WINDOW_MS := 120.0 # timing 誤差在此之內算 GOOD
const LATE_SWING_GRACE_MS := 2500.0 # MISS 後仍接受延遲到達的揮棒封包
const GRAVITY := 18.0         # 球噴飛後的重力
const READY_MIN := 1.0
const READY_MAX := 2.5
const BALL_TRAIL_COUNT := 18
const IMPACT_SPARK_COUNT := 30
const IMPACT_DURATION := 0.45
const SWING_ARC_COUNT := 16

var udp := PacketPeerUDP.new()

var ball: MeshInstance3D
var bat: MeshInstance3D
var ball_trail: Array[MeshInstance3D] = []
var ball_trail_color := Color(0.35, 0.82, 1.0, 0.72)
var impact_sparks: Array[MeshInstance3D] = []
var impact_velocities: Array[Vector3] = []
var impact_age := IMPACT_DURATION
var impact_color := Color(1.0, 0.85, 0.25, 0.9)
var impact_light: OmniLight3D
var swing_arc: Array[MeshInstance3D] = []
var status_label: Label
var detail_label: Label
var hint_label: Label
var result_panel: PanelContainer
var result_title_label: Label
var result_body_label: RichTextLabel

enum State { READY_WAIT, PITCHING, HIT, RESULT }
var state: int = State.READY_WAIT
var state_start_ms: float = 0.0
var ready_delay_ms: float = 0.0
var pitch_time_ms: float = 0.0
var ideal_hit_ms: float = 0.0
var miss_time_ms: float = 0.0
var ball_vel := Vector3.ZERO


func now_ms() -> float:
	return float(Time.get_ticks_msec())


func _ready() -> void:
	_build_scene()
	_start_new_pitch()
	var bind_err := udp.bind(UDP_PORT)
	if bind_err != OK:
		status_label.text = "UDP ERROR"
		detail_label.text = "Cannot bind UDP %d, error %d" % [UDP_PORT, bind_err]
		push_error("Cannot bind swing UDP port %d, error %d" % [UDP_PORT, bind_err])
	else:
		print("Listening for swings on UDP %d" % UDP_PORT)


# ----------------------------------------------------------------------
# 場景建構
# ----------------------------------------------------------------------
func _mat(c: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	if c.a < 1.0:
		m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	return m


func _glow_mat(c: Color, emission_strength: float = 0.8) -> StandardMaterial3D:
	var m := _mat(c)
	m.emission_enabled = true
	m.emission = Color(c.r, c.g, c.b)
	m.emission_energy_multiplier = emission_strength
	return m


func _make_box(size: Vector3, pos: Vector3, material: Material) -> MeshInstance3D:
	var node := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = size
	node.mesh = mesh
	node.position = pos
	node.material_override = material
	add_child(node)
	return node


func _make_label(font_size: int) -> Label:
	var l := Label.new()
	l.add_theme_font_size_override("font_size", font_size)
	l.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	l.set_anchors_preset(Control.PRESET_TOP_WIDE)
	return l


func _build_neon_backdrop() -> void:
	var cyan := _glow_mat(Color(0.14, 0.75, 1.0, 0.82), 1.2)
	var pink := _glow_mat(Color(1.0, 0.16, 0.72, 0.82), 1.2)
	var gold := _glow_mat(Color(1.0, 0.76, 0.22, 0.75), 0.9)

	for x in [-14.0, -10.0, 10.0, 14.0]:
		var mat := cyan if x < 0.0 else pink
		_make_box(Vector3(0.18, 4.4, 0.18), Vector3(x, 2.2, -17.0), mat)
		_make_box(Vector3(1.8, 0.08, 0.08), Vector3(x, 4.45, -17.0), mat)

	for z in [-17.0, -12.0, -7.0]:
		_make_box(Vector3(24.0, 0.04, 0.08), Vector3(0, 0.08, z), gold)

	for i in range(36):
		var dot := MeshInstance3D.new()
		var mesh := SphereMesh.new()
		mesh.radius = 0.035
		mesh.height = 0.07
		dot.mesh = mesh
		dot.position = Vector3(randf_range(-18.0, 18.0), randf_range(4.0, 9.0), randf_range(-26.0, -8.0))
		dot.material_override = _glow_mat(Color(0.72, 0.9, 1.0, 0.62), 0.75)
		add_child(dot)


func _build_scene() -> void:
	# 攝影機:擺在打者後方稍高處,望向場地(Godot 攝影機預設看 -z)
	var cam := Camera3D.new()
	cam.position = Vector3(0, 6, 16)
	cam.rotation_degrees = Vector3(-14, 0, 0)
	cam.fov = 60.0
	add_child(cam)

	# 平行光 + 環境
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-50, -40, 0)
	light.shadow_enabled = true
	add_child(light)

	var we := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.035, 0.048, 0.075)
	env.ambient_light_color = Color(0.58, 0.68, 0.82)
	env.ambient_light_energy = 0.75
	env.glow_enabled = true
	env.glow_intensity = 0.62
	env.glow_strength = 1.15
	we.environment = env
	add_child(we)

	# 地面
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(80, 80)
	ground.mesh = pm
	ground.material_override = _mat(Color(0.09, 0.27, 0.18))
	add_child(ground)

	var dirt_mat := _mat(Color(0.47, 0.34, 0.22))
	var chalk_mat := _glow_mat(Color(0.92, 0.95, 0.82, 0.88), 0.25)
	var grass_line_mat := _mat(Color(0.12, 0.42, 0.25))
	_make_box(Vector3(9.0, 0.025, 7.5), Vector3(0, 0.013, 2.5), dirt_mat)
	var left_foul := _make_box(Vector3(0.08, 0.035, 28.0), Vector3(-2.8, 0.04, -8.0), chalk_mat)
	left_foul.rotation_degrees.y = -6.0
	var right_foul := _make_box(Vector3(0.08, 0.035, 28.0), Vector3(2.8, 0.04, -8.0), chalk_mat)
	right_foul.rotation_degrees.y = 6.0
	for x in [-9.0, -5.5, 5.5, 9.0]:
		_make_box(Vector3(0.08, 0.02, 60.0), Vector3(x, 0.03, -8.0), grass_line_mat)
	_make_box(Vector3(1.4, 0.04, 1.0), Vector3(0, 0.06, 3.0), chalk_mat)
	_build_neon_backdrop()

	# 好球帶(半透明)
	var zone := MeshInstance3D.new()
	var zbox := BoxMesh.new()
	zbox.size = Vector3(2.2, 3.0, 0.1)
	zone.mesh = zbox
	zone.position = Vector3(0, 2.0, HIT_Z)
	var zmat := _glow_mat(Color(0.22, 0.64, 1.0, 0.28), 1.35)
	zone.material_override = zmat
	add_child(zone)
	_make_box(Vector3(2.45, 0.05, 0.08), Vector3(0, 3.55, HIT_Z), _glow_mat(Color(0.4, 0.82, 1.0, 0.78), 1.0))
	_make_box(Vector3(2.45, 0.05, 0.08), Vector3(0, 0.45, HIT_Z), _glow_mat(Color(0.4, 0.82, 1.0, 0.78), 1.0))
	_make_box(Vector3(0.05, 3.1, 0.08), Vector3(-1.25, 2.0, HIT_Z), _glow_mat(Color(0.4, 0.82, 1.0, 0.78), 1.0))
	_make_box(Vector3(0.05, 3.1, 0.08), Vector3(1.25, 2.0, HIT_Z), _glow_mat(Color(0.4, 0.82, 1.0, 0.78), 1.0))

	# 投手丘
	var mound := MeshInstance3D.new()
	var mbox := BoxMesh.new()
	mbox.size = Vector3(4, 0.4, 4)
	mound.mesh = mbox
	mound.position = Vector3(0, 0.2, BALL_START_Z)
	mound.material_override = _mat(Color(0.5, 0.38, 0.25))
	add_child(mound)

	# 球棒
	bat = MeshInstance3D.new()
	var bbox := BoxMesh.new()
	bbox.size = Vector3(0.25, 2.2, 0.25)
	bat.mesh = bbox
	bat.position = Vector3(-1.6, 1.6, 2.0)
	bat.rotation_degrees = Vector3(0, 0, 30)
	bat.material_override = _mat(Color(0.82, 0.82, 0.85))
	add_child(bat)
	_build_swing_arc()

	# 球
	ball = MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 0.4
	sm.height = 0.8
	ball.mesh = sm
	ball.material_override = _glow_mat(Color(1.0, 0.96, 0.78), 0.75)
	ball.position = Vector3(0, 2.0, BALL_START_Z)
	ball.visible = false
	add_child(ball)
	_build_ball_trail()
	_build_impact_fx()

	# UI
	var layer := CanvasLayer.new()
	add_child(layer)

	status_label = _make_label(52)
	status_label.offset_top = 40
	layer.add_child(status_label)

	detail_label = _make_label(22)
	detail_label.offset_top = 120
	layer.add_child(detail_label)

	hint_label = _make_label(24)
	hint_label.set_anchors_preset(Control.PRESET_BOTTOM_WIDE)
	hint_label.offset_top = -70
	layer.add_child(hint_label)

	result_panel = PanelContainer.new()
	result_panel.set_anchors_preset(Control.PRESET_BOTTOM_WIDE)
	result_panel.offset_left = 32
	result_panel.offset_top = -250
	result_panel.offset_right = -32
	result_panel.offset_bottom = -24
	result_panel.visible = false

	var panel_style := StyleBoxFlat.new()
	panel_style.bg_color = Color(0.025, 0.035, 0.052, 0.88)
	panel_style.border_color = Color(0.58, 0.86, 1.0, 0.95)
	panel_style.set_border_width_all(2)
	panel_style.corner_radius_top_left = 8
	panel_style.corner_radius_top_right = 8
	panel_style.corner_radius_bottom_left = 8
	panel_style.corner_radius_bottom_right = 8
	panel_style.shadow_color = Color(0.0, 0.0, 0.0, 0.55)
	panel_style.shadow_size = 12
	result_panel.add_theme_stylebox_override("panel", panel_style)
	layer.add_child(result_panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 24)
	margin.add_theme_constant_override("margin_top", 18)
	margin.add_theme_constant_override("margin_right", 24)
	margin.add_theme_constant_override("margin_bottom", 18)
	result_panel.add_child(margin)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 12)
	margin.add_child(box)

	result_title_label = Label.new()
	result_title_label.add_theme_font_size_override("font_size", 26)
	result_title_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	box.add_child(result_title_label)

	result_body_label = RichTextLabel.new()
	result_body_label.bbcode_enabled = true
	result_body_label.fit_content = false
	result_body_label.scroll_active = false
	result_body_label.custom_minimum_size = Vector2(760, 132)
	result_body_label.add_theme_font_size_override("normal_font_size", 17)
	result_body_label.add_theme_font_size_override("bold_font_size", 17)
	box.add_child(result_body_label)


func _build_ball_trail() -> void:
	var mesh := SphereMesh.new()
	mesh.radius = 0.2
	mesh.height = 0.4

	for i in range(BALL_TRAIL_COUNT):
		var dot := MeshInstance3D.new()
		dot.mesh = mesh
		dot.visible = false
		dot.scale = Vector3.ONE * (1.0 - float(i) / float(BALL_TRAIL_COUNT) * 0.68)

		var alpha := 0.58 * (1.0 - float(i) / float(BALL_TRAIL_COUNT))
		dot.material_override = _glow_mat(Color(ball_trail_color.r, ball_trail_color.g, ball_trail_color.b, alpha), 0.55)
		ball_trail.append(dot)
		add_child(dot)


func _set_ball_trail_color(color: Color) -> void:
	ball_trail_color = color
	for i in range(ball_trail.size()):
		var fade := 1.0 - float(i) / float(BALL_TRAIL_COUNT)
		var dot := ball_trail[i]
		dot.material_override = _glow_mat(Color(color.r, color.g, color.b, color.a * fade), 0.55)


func _clear_ball_trail() -> void:
	for dot in ball_trail:
		dot.visible = false
		dot.position = ball.position


func _update_ball_trail() -> void:
	if ball_trail.is_empty() or not ball.visible:
		return

	for i in range(ball_trail.size() - 1, 0, -1):
		ball_trail[i].position = ball_trail[i - 1].position
		ball_trail[i].visible = ball_trail[i - 1].visible

	ball_trail[0].position = ball.position
	ball_trail[0].visible = true


func _build_impact_fx() -> void:
	var spark_mesh := SphereMesh.new()
	spark_mesh.radius = 0.09
	spark_mesh.height = 0.18

	for i in range(IMPACT_SPARK_COUNT):
		var spark := MeshInstance3D.new()
		spark.mesh = spark_mesh
		spark.visible = false
		spark.material_override = _glow_mat(impact_color, 1.2)
		impact_sparks.append(spark)
		impact_velocities.append(Vector3.ZERO)
		add_child(spark)

	impact_light = OmniLight3D.new()
	impact_light.light_color = Color(1.0, 0.82, 0.35)
	impact_light.light_energy = 0.0
	impact_light.omni_range = 8.0
	add_child(impact_light)


func _build_swing_arc() -> void:
	var arc_mesh := SphereMesh.new()
	arc_mesh.radius = 0.075
	arc_mesh.height = 0.15

	for i in range(SWING_ARC_COUNT):
		var dot := MeshInstance3D.new()
		dot.mesh = arc_mesh
		dot.visible = false
		dot.material_override = _glow_mat(Color(1.0, 0.76, 0.22, 0.85), 1.0)
		swing_arc.append(dot)
		add_child(dot)


func _show_swing_arc(color: Color) -> void:
	for i in range(swing_arc.size()):
		var t: float = float(i) / max(1.0, float(swing_arc.size() - 1))
		var angle: float = lerp(-0.95, 0.95, t)
		var radius: float = 2.45
		var dot: MeshInstance3D = swing_arc[i]
		dot.position = Vector3(-0.25 + cos(angle) * radius, 2.05 + sin(angle) * 1.18, 1.35 - t * 0.85)
		dot.scale = Vector3.ONE * lerp(1.0, 0.28, t)
		dot.material_override = _glow_mat(Color(color.r, color.g, color.b, color.a * (1.0 - t * 0.55)), 1.25)
		dot.visible = true

	var tw := create_tween()
	for dot in swing_arc:
		tw.parallel().tween_property(dot, "scale", Vector3.ZERO, 0.28).set_trans(Tween.TRANS_QUAD).set_ease(Tween.EASE_OUT)
	tw.tween_callback(func():
		for dot in swing_arc:
			dot.visible = false
	)


func _trigger_impact_burst(color: Color) -> void:
	impact_color = color
	impact_age = 0.0
	impact_light.position = ball.position
	impact_light.light_color = Color(color.r, color.g, color.b)
	impact_light.light_energy = 5.0

	for i in range(impact_sparks.size()):
		var angle: float = TAU * float(i) / float(impact_sparks.size())
		var lift: float = randf_range(-0.35, 0.85)
		var dir: Vector3 = Vector3(cos(angle), lift, sin(angle)).normalized()
		impact_velocities[i] = dir * randf_range(5.2, 8.8)
		impact_sparks[i].position = ball.position
		impact_sparks[i].scale = Vector3.ONE * randf_range(0.7, 1.35)
		impact_sparks[i].material_override = _glow_mat(color, 1.45)
		impact_sparks[i].visible = true


func _update_impact_burst(delta: float) -> void:
	if impact_age >= IMPACT_DURATION:
		return

	impact_age += delta
	var fade: float = clamp(1.0 - impact_age / IMPACT_DURATION, 0.0, 1.0)

	if impact_light != null:
		impact_light.light_energy = 5.0 * fade

	for i in range(impact_sparks.size()):
		var spark: MeshInstance3D = impact_sparks[i]
		spark.position += impact_velocities[i] * delta
		spark.position.y -= 3.2 * delta * impact_age
		spark.scale = Vector3.ONE * (0.25 + fade * 1.1)
		spark.material_override = _glow_mat(Color(impact_color.r, impact_color.g, impact_color.b, impact_color.a * fade), 1.2)
		spark.visible = fade > 0.04


# ----------------------------------------------------------------------
# 遊戲流程
# ----------------------------------------------------------------------
func _start_new_pitch() -> void:
	state = State.READY_WAIT
	state_start_ms = now_ms()
	ready_delay_ms = randf_range(READY_MIN, READY_MAX) * 1000.0
	miss_time_ms = 0.0
	ball.visible = false
	ball.position = Vector3(0, 2.0, BALL_START_Z)
	ball_vel = Vector3.ZERO
	_set_ball_trail_color(Color(0.35, 0.82, 1.0, 0.72))
	_clear_ball_trail()
	impact_age = IMPACT_DURATION
	if impact_light != null:
		impact_light.light_energy = 0.0
	for spark in impact_sparks:
		spark.visible = false
	bat.rotation_degrees = Vector3(0, 0, 30)
	status_label.text = "READY"
	detail_label.text = ""
	hint_label.text = "球丟出後再揮棒"
	hint_label.offset_top = -70
	result_panel.visible = false


func _classify(error_ms: float) -> String:
	if abs(error_ms) <= GOOD_WINDOW_MS:
		return "GOOD"
	elif error_ms < -GOOD_WINDOW_MS:
		return "EARLY"
	else:
		return "LATE"


func _range_score(value: float, low: float, high: float) -> float:
	if high <= low:
		return 0.0
	return clamp((value - low) / (high - low) * 100.0, 0.0, 100.0)


func _swing_score(data: Dictionary, error_ms: float) -> int:
	var timing_score: float = clamp(100.0 - abs(error_ms) / GOOD_WINDOW_MS * 40.0, 0.0, 100.0)
	var peak_for_score := float(data.get("filt_peak_dps", data.get("peak_dps", 0)))
	var peak_score: float = _range_score(peak_for_score, 250.0, 900.0)
	var dsp_n := int(data.get("dsp_n", 0))

	if dsp_n <= 0:
		return int(round((timing_score * 0.6) + (peak_score * 0.4)))

	var rms_for_score := float(data.get("filt_rms_dps", data.get("rms_dps", 0)))
	var energy_for_score := float(data.get("filt_energy", data.get("energy", 0)))
	var rms_score: float = _range_score(rms_for_score, 150.0, 650.0)
	var std_score: float = _range_score(float(data.get("std_dps", 0)), 80.0, 500.0)
	var energy: float = max(0.0, energy_for_score)
	var energy_score: float = clamp(log(energy + 1.0) / log(200000.0 + 1.0) * 100.0, 0.0, 100.0)

	return int(round(
		(timing_score * 0.40)
		+ (peak_score * 0.25)
		+ (rms_score * 0.18)
		+ (std_score * 0.07)
		+ (energy_score * 0.10)
	))


func _show_swing_report(
	result: String,
	score: int,
	error_text: String,
	speed: float,
	duration: int,
	peak_dps: int,
	rms_dps: int,
	energy: int,
	filt_peak: int,
	filt_rms: int,
	filt_energy: int,
	mean_dps: int,
	std_dps: int,
	dsp_n: int,
	drop: int,
	dsp_drop: int
) -> void:
	var primary_peak := filt_peak if filt_peak > 0 else peak_dps
	var primary_rms := filt_rms if filt_rms > 0 else rms_dps
	var primary_energy := filt_energy if filt_energy > 0 else energy

	result_title_label.text = "成績 %d  |  %s" % [score, result]
	result_body_label.text = (
		"[b]重點[/b] 時機 %s ms    棒速 %.2f m/s    揮棒時間 %d ms\n"
		+ "[b]力量[/b] 峰值 %d dps    RMS %d dps    能量 %d\n"
		+ "[b]穩定[/b] 平均 %d dps    變化量 %d dps    樣本 %d    掉包 %d/%d\n"
		+ "[b]說明[/b] 時機越接近 0 越準；負值太早、正值太晚。峰值看爆發力，RMS 看整段強度，能量看總用力，變化量看加速是否明顯。"
	) % [
		error_text,
		speed,
		duration,
		primary_peak,
		primary_rms,
		primary_energy,
		mean_dps,
		std_dps,
		dsp_n,
		drop,
		dsp_drop,
	]
	hint_label.offset_top = -286
	result_panel.visible = true


func _on_swing(data: Dictionary) -> void:
	var arrival := now_ms()
	var peak_age: float = float(data.get("peak_age_ms", 0.0))
	var peak_ms: float = arrival - peak_age          # 峰值在 Godot 時間軸的位置
	if not _can_accept_swing(peak_ms, arrival):
		print("Ignored swing: state=%d status=%s peak_age=%.0f" % [state, status_label.text, peak_age])
		return  # 不在打擊時段,或太晚才到的揮棒忽略

	var error_ms: float = peak_ms - ideal_hit_ms
	var result := _classify(error_ms)

	# 揮棒動畫
	var tw := create_tween()
	tw.tween_property(bat, "rotation_degrees:z", -60.0, 0.12) \
		.set_trans(Tween.TRANS_EXPO).set_ease(Tween.EASE_OUT)

	# 用感測到的揮棒速度映射成球的出射速度(純視覺,比例可自己調)
	var speed: float = float(data.get("speed", 0.0))
	var launch: float = max(8.0, speed * 1.6 + 6.0)

	if not ball.visible:
		ball.visible = true
		ball.position = Vector3(0, 2.0, HIT_Z + 1.5)

	match result:
		"GOOD":
			var fx_color := Color(1.0, 0.86, 0.28, 0.9)
			_set_ball_trail_color(fx_color)
			_show_swing_arc(fx_color)
			_trigger_impact_burst(fx_color)
			ball_vel = Vector3(randf_range(-2, 2), launch * 0.6, -launch)
		"EARLY":
			var fx_color := Color(1.0, 0.28, 0.22, 0.82)
			_set_ball_trail_color(fx_color)
			_show_swing_arc(fx_color)
			_trigger_impact_burst(fx_color)
			ball_vel = Vector3(-launch * 0.7, launch * 0.4, -launch * 0.5)
		_:  # LATE
			var fx_color := Color(0.45, 0.72, 1.0, 0.82)
			_set_ball_trail_color(fx_color)
			_show_swing_arc(fx_color)
			_trigger_impact_burst(fx_color)
			ball_vel = Vector3(launch * 0.7, launch * 0.4, -launch * 0.5)

	var es := ("+" if error_ms >= 0.0 else "") + ("%.0f" % error_ms)
	var score := _swing_score(data, error_ms)
	var peak_dps := int(data.get("peak_dps", 0))
	var rms_dps := int(data.get("rms_dps", 0))
	var energy := int(data.get("energy", 0))
	var mean_dps := int(data.get("mean_dps", 0))
	var std_dps := int(data.get("std_dps", 0))
	var filt_rms := int(data.get("filt_rms_dps", 0))
	var filt_energy := int(data.get("filt_energy", 0))
	var filt_peak := int(data.get("filt_peak_dps", 0))
	var dsp_n := int(data.get("dsp_n", 0))
	var drop := int(data.get("drop", 0))
	var dsp_drop := int(data.get("dsp_drop", 0))
	status_label.text = result
	detail_label.text = "成績 %d | err %s ms | %.2f m/s | %d ms" % [
		score,
		es,
		speed,
		int(data.get("duration", 0)),
	]
	_show_swing_report(
		result,
		score,
		es,
		speed,
		int(data.get("duration", 0)),
		peak_dps,
		rms_dps,
		energy,
		filt_peak,
		filt_rms,
		filt_energy,
		mean_dps,
		std_dps,
		dsp_n,
		drop,
		dsp_drop
	)
	hint_label.text = "SPACE 下一球"
	state = State.HIT


func _can_accept_swing(peak_ms: float, arrival_ms: float) -> bool:
	if state == State.PITCHING:
		return true

	if state == State.RESULT and status_label.text == "MISS":
		if arrival_ms - miss_time_ms <= LATE_SWING_GRACE_MS:
			return peak_ms >= pitch_time_ms

	return false


func _result_miss() -> void:
	status_label.text = "MISS"
	detail_label.text = "沒偵測到揮棒"
	hint_label.text = "SPACE 下一球"
	hint_label.offset_top = -70
	result_panel.visible = false
	ball.visible = false
	_clear_ball_trail()
	miss_time_ms = now_ms()
	state = State.RESULT


func _process(delta: float) -> void:
	_update_impact_burst(delta)

	# 收 UDP 揮棒事件(非阻塞)
	while udp.get_available_packet_count() > 0:
		var txt := udp.get_packet().get_string_from_utf8()
		var data = JSON.parse_string(txt)
		if typeof(data) == TYPE_DICTIONARY and data.get("type") == "swing":
			print("Received swing UDP: %s" % txt)
			_on_swing(data)
		else:
			print("Ignored UDP packet: %s" % txt)

	var t := now_ms()

	match state:
		State.READY_WAIT:
			if t - state_start_ms >= ready_delay_ms:
				state = State.PITCHING
				pitch_time_ms = t
				var dist: float = abs(HIT_Z - BALL_START_Z)
				ideal_hit_ms = pitch_time_ms + dist / BALL_SPEED * 1000.0
				ball.visible = true
				ball.position = Vector3(0, 2.0, BALL_START_Z)
				_clear_ball_trail()
				status_label.text = "PITCH!"
				hint_label.text = "揮棒!"

		State.PITCHING:
			var elapsed: float = (t - pitch_time_ms) / 1000.0
			ball.position.z = BALL_START_Z + BALL_SPEED * elapsed
			_update_ball_trail()
			if ball.position.z > HIT_Z + 4.0:
				_result_miss()  # 球過了還沒揮

		State.HIT:
			ball_vel.y -= GRAVITY * delta
			ball.position += ball_vel * delta
			_update_ball_trail()
			if ball.position.y <= 0.4:
				ball.position.y = 0.4
				state = State.RESULT

		State.RESULT:
			pass


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_SPACE:
			_start_new_pitch()
		elif event.keycode == KEY_ESCAPE:
			get_tree().quit()
