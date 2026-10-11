import sys

def bin2c(input_file, output_file, array_name):
    with open(input_file, 'rb') as f:
        data = f.read()

    lines = []
    lines.append('#pragma once')
    lines.append('#include <Arduino.h>')
    lines.append('')
    lines.append(f'const uint8_t {array_name}[] PROGMEM = {{')
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ', '.join(f'0x{b:02X}' for b in chunk)
        if i + 16 < len(data):
            lines.append(f'  {hex_str},')
        else:
            lines.append(f'  {hex_str}')
    lines.append('};')
    lines.append(f'const size_t {array_name}_len = {len(data)};')

    with open(output_file, 'w') as f:
        f.write('\n'.join(lines))

    print(f'Done! {len(data)} bytes -> {output_file}')
    print(f'  Array name: {array_name}')

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print('用法: python font_to_c.py <字体文件> <输出.h> [数组名]')
        print('示例: python font_to_c.py font.ttf font_array.h font_data')
        sys.exit(1)

    src = sys.argv[1]
    dst = sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else 'font_data'

    bin2c(src, dst, name)