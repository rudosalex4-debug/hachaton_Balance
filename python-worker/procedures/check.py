"""Example user CHECK: changes in the final decision."""

def run(mart, params=None):
    params = {**CHECK['params'], **(params or {})}
    apps = mart.app_ids(path='decisionResult.decision', diff_flag=1)
    total = mart.total_apps()
    return [{'LABEL': CHECK['label'], 'SOURCE': 'AFTER',
             'ARGUMENT': 'decisionResult.decision', 'CNT': len(apps), 'TOTAL': total,
             'METRIC': len(apps) / total if total else 0.0,
             'APP_IDS': apps[:params['app_ids_limit']],
             'RESULT': 'FAIL' if apps else 'PASS',
             'CONFIDENCE': 'LOW' if 0 < len(apps) < params['low_confidence_cnt'] else 'HIGH'}]


CHECK = {'id': 'decision_changed', 'label': 'Final decision changed',
         'params': {'app_ids_limit': 20, 'low_confidence_cnt': 5}, 'entrypoint': run}
