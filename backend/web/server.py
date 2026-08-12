import dataclasses
import sqlite3

from flask import (
    Flask, Response, make_response, render_template, request, redirect,
)

from app_state import AppState
# Imported as a module, not `from config.settings import WEB_PORT`: read at call
# time the setting can be replaced in a test without reloading every importer.
from config import settings
from protocol.packet import (
    SAMPLING_CODES, UINT16_MAX, sampling_code_from_hz, sampling_hz_from_code,
)

# Fixed order — the error message lists offenders in form order, never in dict
# iteration order, so the same bad input always produces the same message.
FORM_FIELDS = ['sleep_min', 'idle_min', 'max_acq', 'cooldown_sec']


def validate_config_form(form) -> tuple[dict, list]:
    """Validate all four config fields, returning (values, errors).

    Every offender is reported, one line per field, each naming the field and
    the reason: the operator is at a laptop in a plant, and a second rejection
    after fixing the first one reads as a broken form.
    """
    values, errors = {}, []
    for field in FORM_FIELDS:
        raw = form.get(field)
        if raw is None:
            errors.append(f'{field}: campo ausente.')
            continue
        try:
            value = int(raw)
        except ValueError:
            errors.append(f'{field}: "{raw}" nao e um numero inteiro.')
            continue
        if value <= 0:
            errors.append(f'{field}: deve ser maior que zero.')
        elif value > UINT16_MAX:
            errors.append(
                f'{field}: {value} nao cabe no campo do protocolo '
                f'(maximo {UINT16_MAX}).'
            )
        else:
            values[field] = value
    return values, errors


def _storage_unavailable(error: sqlite3.OperationalError) -> Response:
    """Answer a write that the database refused.

    Disk full, file turned read-only, lock held past the timeout: none of it
    means the server is broken — the sensors keep transmitting on the last saved
    configuration. A bare 500 would read as "everything is down" and send the
    operator chasing the wrong thing, so the reason is said out loud, and the
    status says try again rather than "you sent something wrong".
    """
    return Response(
        f'Nao foi possivel salvar a configuracao: {error}. '
        f'A configuracao anterior continua valendo e os sensores seguem '
        f'transmitindo com ela.',
        status=503, mimetype='text/plain',
    )


def create_app(state: AppState) -> Flask:
    app = Flask(__name__)

    @app.route('/')
    def index():
        with state.lock:
            config      = dataclasses.replace(state.config)
            connections = list(state.connections)
            ota_armed   = state.ota_armed
        # Which device took the flag last, derived from the history already
        # copied — no new state. It does NOT say whether that device came back:
        # the only key we have is a DHCP address, so a device returning on a new
        # lease reads as a different one and a reused address as the same one.
        # An inference that fails exactly when the network is unusual is worse
        # than none, because it would be believed.
        last_ota = next((e for e in reversed(connections) if e.ota_sent), None)
        page = make_response(render_template(
            'index.html', config=config, connections=connections,
            ota_armed=ota_armed, last_ota=last_ota,
            sampling_hz=sampling_hz_from_code(config.sampling_code),
            sampling_options=list(SAMPLING_CODES),
        ))
        # The page is the operator's only instrument: a cached render showing
        # a stale arming state is worse than no page at all.
        page.headers['Cache-Control'] = 'no-store'
        return page

    @app.route('/ota', methods=['POST'])
    def update_ota():
        """Arm or disarm the one-shot OTA flag — separate from the config form
        so saving an unrelated field can never arm or disarm a device."""
        armed = request.form.get('armed')
        if armed not in ('0', '1'):
            reason = ('armed: campo ausente.' if armed is None
                      else f'armed: "{armed}" nao e um valor valido.')
            return Response(
                f'{reason} Use 1 para armar o OTA ou 0 para desarmar.',
                status=400, mimetype='text/plain',
            )

        state.set_ota_armed(armed == '1')
        return redirect('/', 303)

    @app.route('/sampling', methods=['POST'])
    def update_sampling():
        """Change the acquisition rate of the whole fleet.

        A route of its own, like /ota and for the same reason: this one save
        re-rates every sensor in the plant, and it must never ride along with a
        distracted save of an unrelated field.
        """
        raw = request.form.get('sampling_hz')
        if raw is None:
            return Response(
                'sampling_hz: campo ausente. Use uma das taxas: '
                + ', '.join(SAMPLING_CODES) + '.',
                status=400, mimetype='text/plain',
            )

        try:
            code = sampling_code_from_hz(raw)
        except ValueError as invalid:
            return Response(str(invalid), status=400, mimetype='text/plain')

        try:
            state.update_config(sampling_code=code)
        except sqlite3.OperationalError as unavailable:
            return _storage_unavailable(unavailable)
        return redirect('/', 303)

    @app.route('/config', methods=['POST'])
    def update_config():
        values, errors = validate_config_form(request.form)
        if errors:
            # Nothing is written when anything is wrong: a partially applied
            # config is one no operator ever chose, shown as if they had.
            return Response('\n'.join(errors), status=400, mimetype='text/plain')

        # The rate is deliberately absent from this call: saving the duty cycle
        # must not re-rate the fleet. See update_sampling above.
        try:
            state.update_config(
                sleep_min=values['sleep_min'], idle_min=values['idle_min'],
                max_acq=values['max_acq'], cooldown_sec=values['cooldown_sec'],
            )
        except sqlite3.OperationalError as unavailable:
            return _storage_unavailable(unavailable)

        return redirect('/', 303)

    return app


def web_server_main(state: AppState, port: int | None = None) -> None:
    app = create_app(state)
    app.run(
        host='0.0.0.0',
        port=settings.WEB_PORT if port is None else port,
        use_reloader=False,
    )
