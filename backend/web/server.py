import dataclasses
from flask import Flask, Response, render_template, request, redirect

from app_state import AppState
from protocol.packet import UINT16_MAX

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


def create_app(state: AppState) -> Flask:
    app = Flask(__name__)

    @app.route('/')
    def index():
        with state.lock:
            config = dataclasses.replace(state.config)
            connections = list(state.connections)
        return render_template('index.html', config=config, connections=connections)

    @app.route('/config', methods=['POST'])
    def update_config():
        values, errors = validate_config_form(request.form)
        if errors:
            # Nothing is written when anything is wrong: a partially applied
            # config is one no operator ever chose, shown as if they had.
            return Response('\n'.join(errors), status=400, mimetype='text/plain')

        with state.lock:
            state.config.sleep_min    = values['sleep_min']
            state.config.idle_min     = values['idle_min']
            state.config.max_acq      = values['max_acq']
            state.config.cooldown_sec = values['cooldown_sec']

        return redirect('/', 303)

    return app


def web_server_main(state: AppState, port: int = 8080) -> None:
    app = create_app(state)
    app.run(host='0.0.0.0', port=port, use_reloader=False)
