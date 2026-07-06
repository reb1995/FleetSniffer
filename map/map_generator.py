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
            if len(parts) == 11:
                coords = parts[10].split(',')
                if len(coords) == 2:
                    lat = float(coords[0].strip())
                    lon = float(coords[1].strip())
                    mac = parts[0]
                    rssi = parts[5]
                    chan = parts[6]
                    ssid = parts[7]
                    vendor = parts[8]
                    timestamp = parts[9].strip('[]') + " UTC"
                    popup_text = f"<b>SSID:</b> {ssid}<br><b>MAC:</b> {mac}<br><b>RSSI:</b> {rssi}<br><b>Vendor</b> {vendor}<br><b>Chan:</b> {chan}<br><b>Time:</b> {timestamp}"
                    folium.Marker(
                        [lat, lon],
                        popup=popup_text,
                        tooltip=ssid
                    ).add_to(m)
                    bounds.append([lat, lon])
            if len(parts) == 10:
                coords = parts[9].split(',')
                if len(coords) == 2:
                    lat = float(coords[0].strip())
                    lon = float(coords[1].strip())
                    mac = parts[0]
                    rssi = parts[5]
                    chan = parts[6]
                    ssid = parts[7]
                    vendor = "N/A" # Before vendor tag added
                    timestamp = parts[8].strip('[]') + " UTC"
                    popup_text = f"<b>SSID:</b> {ssid}<br><b>MAC:</b> {mac}<br><b>RSSI:</b> {rssi}<br><b>Vendor</b> {vendor}<br><b>Chan:</b> {chan}<br><b>Time:</b> {timestamp}"
                    folium.Marker(
                        [lat, lon],
                        popup=popup_text,
                        tooltip=ssid
                    ).add_to(m)
                    bounds.append([lat, lon])
            if len(parts) == 9:
                coords = parts[8].split(',')
                if len(coords) == 2:
                    lat = float(coords[0].strip())
                    lon = float(coords[1].strip())
                    mac = parts[0]
                    rssi = parts[5]
                    chan = -1 # Detections before channel added.
                    ssid = parts[6]
                    vendor = "N/A" # Before vendor tag added
                    timestamp = parts[7].strip('[]') + " UTC"
                    popup_text = f"<b>SSID:</b> {ssid}<br><b>MAC:</b> {mac}<br><b>RSSI:</b> {rssi}<br><b>Vendor</b> {vendor}<br><b>Chan:</b> {chan}<br><b>Time:</b> {timestamp}"
                    folium.Marker(
                        [lat, lon],
                        popup=popup_text,
                        tooltip=ssid
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
