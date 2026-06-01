import folium
import sys

def generate_osm_map(log_file, output_html):
    m = folium.Map(location=[0, 0], zoom_start=2, tiles="OpenStreetMap")
    bounds = []
    with open(log_file, 'r') as f:
        for line in f:
            if '---' in line or not line.strip():
                continue
            parts = [p.strip() for p in line.split('|')]
            if len(parts) >= 9:
                coords = parts[8].split(',')
                if len(coords) == 2:
                    lat = float(coords[0].strip())
                    lon = float(coords[1].strip())
                    mac = parts[0]
                    rssi = parts[5]
                    ssid = parts[6]
                    popup_text = f"<b>SSID:</b> {ssid}<br><b>MAC:</b> {mac}<br><b>RSSI:</b> {rssi}"
                    folium.Marker(
                        [lat, lon],
                        popup=popup_text,
                        tooltip=mac
                    ).add_to(m)
                    bounds.append([lat, lon])
    if bounds:
        m.fit_bounds(bounds)
    m.save(output_html)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python map_generator.py <path_to_log_file>")
        sys.exit(1)

    input_log = sys.argv[1]
    generate_osm_map(input_log, 'index.html')
