"""Replaceable hash aggregation; count unique applications, not attributes."""
import json


def aggregate(comparison_rows, union_rows, segments, config, emit):
    dimensions = config.get('aggregate_dimensions', [
        'PREFIX', 'CLASS', 'SUFFIX', 'ARRAY_KEY', 'KEY', 'PATH',
        'VALUE_ADJUSTED', 'SOURCE', 'DIFF_FLAG', 'CHANGE_TYPE',
        'PRODUCTS', 'PARTNER_TYPE', 'ROUTING_METHOD', 'DECISION_CHANGED'])
    context_by_app = {row['APP_ID']: row for row in segments}
    groups = {}
    for row in comparison_rows:
        for source in ('BEFORE', 'AFTER'):
            context = dict(context_by_app[row['APP_ID']], **row)
            context['SOURCE'] = source
            context['VALUE_ADJUSTED'] = row['ADJUSTED_' + source] if row['PRESENT_' + source] else '(отсутствует)'
            context['ADJUSTED_VALUE'] = context['VALUE_ADJUSTED']
            context['VALUE'] = row['VALUE_' + source] if row['PRESENT_' + source] else '(отсутствует)'
            context['VALUE_TYPE'] = row['TYPE_' + source]
            fact = {name: context[name] for name in dimensions}
            key = json.dumps(fact, sort_keys=True, ensure_ascii=False)
            if key not in groups:
                groups[key] = fact, set()
            groups[key][1].add(row['APP_ID'])
    for key in sorted(groups):
        fact, ids = groups[key]
        fact['APP_IDS'] = sorted(ids)
        fact['CNT'] = fact['counts'] = len(ids)
        emit(fact)
