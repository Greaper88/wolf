"""Hardware integration test for a disposable Wolf server ONLY.

Run inside a network-isolated Wolf container with two verified VA GPUs, three
throwaway paired clients and a first app running an animated Wayland client.
The server must expose its default internal ports and Unix socket. The test
creates/stops sessions and a sub-app, and exercises real HEVC RTP output.
For the strict zero-copy fixture, set WOLF_GPU_TEST_REQUIRE_ZERO_COPY=1 and
WOLF_GPU_EXPECT_ZERO_COPY_NODE to the sole verified zero-copy GPU's render node.
That mode verifies streaming on it and rejection while its first encoder is pending.
Never run against user sessions. Set WOLF_GPU_DISPOSABLE_TEST=1 explicitly.
"""
import os
if os.environ.get("WOLF_GPU_DISPOSABLE_TEST") != "1":
    raise SystemExit("Use only a disposable server; set WOLF_GPU_DISPOSABLE_TEST=1")
import http.client, socket, json, time, struct, select
class UnixHTTP(http.client.HTTPConnection):
    def connect(self):
        self.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); self.sock.settimeout(25)
        self.sock.connect(os.environ.get('WOLF_SOCKET_PATH','/var/run/wolf/wolf.sock'))
def request(path,data=None):
    c=UnixHTTP('localhost'); c.request('GET' if data is None else 'POST','/api/v1/'+path,
        None if data is None else json.dumps(data),{'Content-Type':'application/json'})
    r=c.getresponse(); out=r.status,json.loads(r.read()); c.close(); return out
def ok(path,data=None):
    status,body=request(path,data); assert status==200,(path,status,body); return body
clients=ok('clients')['clients']; app=ok('apps')['apps'][0]; sid=[x['client_id'] for x in clients]
def session_data(i):
    return dict(client_ip='127.0.0.1',aes_key='00'*16,aes_iv='0',rtsp_fake_ip=f'127.0.0.{i+2}',
        video_width=640,video_height=360,video_refresh_rate=30,audio_channel_count=2,
        app_id=app['id'],client_id=sid[i])
def add(i):
    data=session_data(i)
    deadline=time.monotonic()+25
    while True:
        status,body=request('sessions/add',data)
        if status==200:return
        assert status==503,(status,body)
        if time.monotonic()>deadline:raise AssertionError(body)
        time.sleep(.25)
def sessions():return ok('sessions')['sessions']
def gpu(i):return next(s['gpu']['id'] for s in sessions() if s['client_id']==sid[i])
def pause(i):return ok('sessions/pause',dict(session_id=sid[i]))
def stop(i):return request('sessions/stop',dict(session_id=sid[i]))
seq=0
def rtsp(i,method,uri='streamid=video/0/0',body=b''):
    global seq;seq+=1
    raw=(f'{method} {uri} RTSP/1.0\r\nCSeq: {seq}\r\nHost: 127.0.0.{i+2}\r\nContent-length: {len(body)}\r\n\r\n').encode()+body
    c=socket.create_connection(('127.0.0.1',48010),timeout=10);c.sendall(raw);out=b''
    while True:
        data=c.recv(65536)
        if not data:break
        out+=data
    c.close();assert b'200 OK' in out,out;return out
sockets=[]
def stream(i):
    response=rtsp(i,'SETUP');secret=next(line.split(b':',1)[1].strip() for line in response.split(b'\r\n') if line.lower().startswith(b'x-ss-ping-payload:'))
    body=('v=0\r\na=x-nv-video[0].clientViewportWd:640\r\na=x-nv-video[0].clientViewportHt:360\r\n'
          'a=x-nv-video[0].maxFPS:30\r\na=x-nv-vqos[0].bitStreamFormat:1\r\na=x-nv-general.featureFlags:0\r\n').encode()
    rtsp(i,'ANNOUNCE','streamid=control/13/0',body)
    udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind(('127.0.0.1',0));udp.settimeout(8)
    udp.sendto(secret+struct.pack('<I',1),('127.0.0.1',48100));sockets.append(udp)
    return udp
def frames(udp,seconds=1.5):
    deadline=time.monotonic()+seconds;values=[]
    while time.monotonic()<deadline:
        if select.select([udp],[],[],.2)[0]:
            p=udp.recv(65536)
            if len(p)>24:values.append(struct.unpack_from('<I',p,20)[0])
    assert values,'No encoded video packets received';return values
lobby=None
try:
    if os.environ.get('WOLF_GPU_TEST_REQUIRE_ZERO_COPY') == '1':
        add(0)
        current=next(s for s in sessions() if s['client_id']==sid[0])
        assert current['gpu']['render_node']==os.environ['WOLF_GPU_EXPECT_ZERO_COPY_NODE'],current['gpu']
        deadline=time.monotonic()+25
        while True:
            status,body=request('sessions/add',session_data(1))
            if status!=503 or 'verification in progress' not in json.dumps(body).lower():break
            assert time.monotonic()<deadline,body
            time.sleep(.25)
        assert status==503,(status,body)
        assert len(sessions())==1,'strict policy must not fall back to the other GPU'
        frames(stream(0))
        print('PASS: strict zero-copy selects the verified GPU, encodes frames, and excludes the fallback GPU',flush=True)
        raise SystemExit(0)
    add(0);add(1);g0,g1=gpu(0),gpu(1);assert g0!=g1,(g0,g1)
    u0=stream(0);u1=stream(1);frames(u0);frames(u1)
    for current in sessions():
        assert current['gpu']['codec']=='HEVC',current['gpu']
        assert current['gpu']['name'] and '0x' not in current['gpu']['name'],current['gpu']
        usage=current['gpu'].get('gpu_percent')
        assert usage is None or 0 <= float(usage) <= 100,current['gpu']
    print('PASS: session display metadata',[
        {key:s['gpu'].get(key) for key in ('name','render_node','codec','gpu_percent')}
        for s in sessions()],flush=True)
    print('PASS: both launchers encode on separate GPUs',g0,g1,flush=True)
    payload=dict(source_session_id=sid[0],profile_id='gpu-profile',name='Persistent test app',multi_user=False,
        stop_when_everyone_leaves=False,runner_state_folder='profile-data/gpu-profile/test-app',
        runner=dict(type='process',run_cmd='gst-launch-1.0 -q videotestsrc is-live=true pattern=ball ! videoconvert ! waylandsink sync=false'),
        video_settings=dict(width=640,height=360,refresh_rate=30,wayland_render_node='/dev/dri/renderD128',runner_render_node='/dev/dri/renderD128',video_producer_buffer_caps='video/x-raw'),
        audio_settings=dict(channel_count=2),client_settings={})
    lobby=ok('lobbies/create',payload)['lobby_id']
    stored=next(l for l in ok('lobbies')['lobbies'] if l['id']==lobby)
    assert stored['gpu']['id']==g0,stored
    join=lambda i:request('lobbies/join',dict(lobby_id=lobby,moonlight_session_id=sid[i],profile_id='gpu-profile'))
    assert request('lobbies/join',dict(lobby_id=lobby,moonlight_session_id=sid[1],profile_id='wrong-profile'))[0]!=200
    assert gpu(1)==g1,'wrong profile must not change viewer GPU'
    assert join(0)[0]==200
    frames(u0);time.sleep(1)
    assert join(1)[0]!=200,'second viewer must be rejected while owner connected'
    pause(0);time.sleep(.5)
    payload['source_session_id']=sid[1]
    assert ok('lobbies/create',payload)['lobby_id']==lobby,'reconnect must reuse persistent app'
    before=frames(u1)[-1]
    status,body=join(1);assert status==200,(status,body)
    time.sleep(1);after=frames(u1)
    assert max(after)>before,(before,after[-10:])
    assert gpu(1)==g0,'viewer encoder must follow app GPU'
    current=next(s for s in sessions() if s['client_id']==sid[1])
    assert current['gpu']['codec']=='HEVC',current['gpu']
    print('PASS: cross-device join reuses app and moves encoder to its GPU with continuing frames',flush=True)
    ok('lobbies/leave',dict(lobby_id=lobby,moonlight_session_id=sid[1]));time.sleep(1)
    frames(u1);assert gpu(1)==g1,'return must restore launcher GPU'
    print('PASS: return to launcher restores original GPU',flush=True)
    stop(0);time.sleep(.5)
    assert any(l['id']==lobby and l['gpu']['id']==g0 for l in ok('lobbies')['lobbies'])
    print('PASS: persistent app survives original launcher shutdown',flush=True)
finally:
    for i in range(2):stop(i)
    if lobby:request('lobbies/stop',dict(lobby_id=lobby))
    for udp in sockets:udp.close()
