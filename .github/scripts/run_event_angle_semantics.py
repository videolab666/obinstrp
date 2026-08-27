from pathlib import Path

script_path = Path(__file__).with_name("apply_event_angle_semantics.py")
source = script_path.read_text(encoding="utf-8")
source = source.replace(
    'EventDock.AngleAuto.Tooltip="No hay un ángulo preferido guardado para este Event. El Cue manual usa la selección Camera actual; las secuencias automáticas pueden mantener el ángulo actual del bus y usar otra cámara con cobertura disponible."',
    'EventDock.AngleAuto.Tooltip="Este Event no tiene un ángulo preferido guardado. Cue manual usa la selección Camera actual; las secuencias automáticas pueden mantener el ángulo del bus y usar otra cámara con cobertura si hace falta."',
)
source = source.replace(
    'EventDock.AnglePreferred.Tooltip="Este Event está fijado a %1 como su ángulo preferido de reproducción. Haz clic en otro ángulo para previsualizarlo; pulsa ★ Preferred para guardar otro."',
    'EventDock.AnglePreferred.Tooltip="Este Event está fijado a %1 como ángulo preferido. Podés previsualizar otro ángulo y guardarlo con ★ Preferred."',
)
source = source.replace(
    'EventDock.AnglePreviewHint="Haz clic para previsualizar este ángulo en el playhead actual. Esto no cambia el ángulo Preferred del Event hasta que pulses ★ Preferred."',
    'EventDock.AnglePreviewHint="Hacé clic para previsualizar este ángulo en el cursor actual. No cambia Preferred hasta que presiones ★ Preferred."',
)
namespace = {"__file__": str(script_path), "__name__": "__main__"}
exec(compile(source, str(script_path), "exec"), namespace)
