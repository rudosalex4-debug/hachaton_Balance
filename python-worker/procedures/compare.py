"""Standalone comparison of typed values. Buckets never decide equality."""
from decimal import Decimal


def compare(union_rows, config, emit):
    before, after = {}, {}
    for row in union_rows:
        if row['SOURCE'] not in ('BEFORE', 'AFTER'):
            raise ValueError('Invalid SOURCE')
        target = before if row['SOURCE'] == 'BEFORE' else after
        key = row['APP_ID'], row['PATH'], row['ARRAY_KEY']
        if key in target:
            raise ValueError('Duplicate normalized attribute')
        target[key] = row
    aliases = {'number': 'NUMBER', 'int': 'NUMBER', 'float': 'NUMBER', 'null': 'NULL', 'bool': 'BOOLEAN', 'str': 'STRING'}

    def typed(row):
        kind = aliases.get(row['VALUE_TYPE'], row['VALUE_TYPE'])
        return kind, Decimal(row['VALUE']) if kind == 'NUMBER' else row['VALUE']

    for key in sorted(before.keys() | after.keys()):
        b, a = before.get(key), after.get(key)
        change = ('ATTRIBUTE_NEW' if b is None else 'ATTRIBUTE_LOST' if a is None else
                  'SAME' if typed(b) == typed(a) else 'VALUE_CHANGED')
        row = {name: (b or a)[name] for name in ('APP_ID', 'PATH', 'PREFIX', 'CLASS', 'SUFFIX', 'KEY', 'ARRAY_KEY')}
        for side, source in (('BEFORE', b), ('AFTER', a)):
            row['PRESENT_' + side] = source is not None
            row['TYPE_' + side] = typed(source)[0] if source is not None else ''
            value = source['VALUE'] if source is not None else ''
            if source is not None and typed(source)[0] == 'NUMBER':
                number = typed(source)[1]
                value = format(number, 'f')
                if '.' in value:
                    value = value.rstrip('0').rstrip('.')
                if number == 0:
                    value = '0'
            row['VALUE_' + side] = value
            row['ADJUSTED_' + side] = source['ADJUSTED_VALUE'] if source is not None else ''
        row.update(CHANGE_TYPE=change, DIFF_FLAG=int(change != 'SAME'))
        emit(row)
