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
const LATE_SWING_GRACE_MS := 900.0 # MISS 後仍接受延遲到達的揮棒封包
const GRAVITY := 18.0         # 球噴飛後的重力
const READY_MIN := 1.0
const READY_MAX := 2.5

var udp := PacketPeerUDP.new()

var ball: MeshInstance3D
var bat: MeshInstance3D
var status_label: Label
var detail_label: Label
var hint_label: Label

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
	udp.bind(UDP_PORT)
	print("Listening for swings on UDP %d" % UDP_PORT)
	_start_new_pitch()


# ----------------------------------------------------------------------
# 場景建構
# ----------------------------------------------------------------------
func _mat(c: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	return m


func _make_label(font_size: int) -> Label:
	var l := Label.new()
	l.add_theme_font_size_override("font_size", font_size)
	l.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	l.set_anchors_preset(Control.PRESET_TOP_WIDE)
	return l


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
	env.background_color = Color(0.08, 0.09, 0.12)
	env.ambient_light_color = Color(0.45, 0.45, 0.5)
	env.ambient_light_energy = 0.6
	we.environment = env
	add_child(we)

	# 地面
	var ground := MeshInstance3D.new()
	var pm := PlaneMesh.new()
	pm.size = Vector2(80, 80)
	ground.mesh = pm
	ground.material_override = _mat(Color(0.15, 0.35, 0.18))
	add_child(ground)

	# 好球帶(半透明)
	var zone := MeshInstance3D.new()
	var zbox := BoxMesh.new()
	zbox.size = Vector3(2.2, 3.0, 0.1)
	zone.mesh = zbox
	zone.position = Vector3(0, 2.0, HIT_Z)
	var zmat := _mat(Color(0.3, 0.5, 1.0, 0.22))
	zmat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	zone.material_override = zmat
	add_child(zone)

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

	# 球
	ball = MeshInstance3D.new()
	var sm := SphereMesh.new()
	sm.radius = 0.4
	sm.height = 0.8
	ball.mesh = sm
	ball.material_override = _mat(Color(1, 1, 1))
	ball.position = Vector3(0, 2.0, BALL_START_Z)
	ball.visible = false
	add_child(ball)

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
	bat.rotation_degrees = Vector3(0, 0, 30)
	status_label.text = "READY"
	detail_label.text = ""
	hint_label.text = "球丟出後再揮棒"


func _classify(error_ms: float) -> String:
	if abs(error_ms) <= GOOD_WINDOW_MS:
		return "GOOD"
	elif error_ms < -GOOD_WINDOW_MS:
		return "EARLY"
	else:
		return "LATE"


func _on_swing(data: Dictionary) -> void:
	var arrival := now_ms()
	var peak_age: float = float(data.get("peak_age_ms", 0.0))
	var peak_ms: float = arrival - peak_age          # 峰值在 Godot 時間軸的位置
	if not _can_accept_swing(peak_ms, arrival):
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
			ball_vel = Vector3(randf_range(-2, 2), launch * 0.6, -launch)
		"EARLY":
			ball_vel = Vector3(-launch * 0.7, launch * 0.4, -launch * 0.5)
		_:  # LATE
			ball_vel = Vector3(launch * 0.7, launch * 0.4, -launch * 0.5)

	var es := ("+" if error_ms >= 0.0 else "") + ("%.0f" % error_ms)
	status_label.text = result
	detail_label.text = "err %s ms | %d dps | %.2f m/s | %d ms | drop %d" % [
		es,
		int(data.get("peak_dps", 0)),
		speed,
		int(data.get("duration", 0)),
		int(data.get("drop", 0)),
	]
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
	ball.visible = false
	miss_time_ms = now_ms()
	state = State.RESULT


func _process(delta: float) -> void:
	# 收 UDP 揮棒事件(非阻塞)
	while udp.get_available_packet_count() > 0:
		var txt := udp.get_packet().get_string_from_utf8()
		var data = JSON.parse_string(txt)
		if typeof(data) == TYPE_DICTIONARY and data.get("type") == "swing":
			_on_swing(data)

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
				status_label.text = "PITCH!"
				hint_label.text = "揮棒!"

		State.PITCHING:
			var elapsed: float = (t - pitch_time_ms) / 1000.0
			ball.position.z = BALL_START_Z + BALL_SPEED * elapsed
			if ball.position.z > HIT_Z + 4.0:
				_result_miss()  # 球過了還沒揮

		State.HIT:
			ball_vel.y -= GRAVITY * delta
			ball.position += ball_vel * delta
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
