const HID_NAMES={0x04:"A",0x05:"B",0x06:"C",0x07:"D",0x08:"E",0x09:"F",0x0A:"G",0x0B:"H",0x0C:"I",0x0D:"J",0x0E:"K",0x0F:"L",0x10:"M",0x11:"N",0x12:"O",0x13:"P",0x14:"Q",0x15:"R",0x16:"S",0x17:"T",0x18:"U",0x19:"V",0x1A:"W",0x1B:"X",0x1C:"Y",0x1D:"Z",0x1E:"1",0x1F:"2",0x20:"3",0x21:"4",0x22:"5",0x23:"6",0x24:"7",0x25:"8",0x26:"9",0x27:"0",0x28:"Enter",0x29:"Esc",0x2A:"Backspace",0x2B:"Tab",0x2C:"Space",0x2D:"-",0x2E:"=",0x2F:"[",0x30:"]",0x31:"\\\\",0x33:";",0x34:"'",0x35:"`",0x36:",",0x37:".",0x38:"/",0x39:"CapsLock",0x3A:"F1",0x3B:"F2",0x3C:"F3",0x3D:"F4",0x3E:"F5",0x3F:"F6",0x40:"F7",0x41:"F8",0x42:"F9",0x43:"F10",0x44:"F11",0x45:"F12",0x46:"PrintScreen",0x47:"ScrollLock",0x48:"Pause",0x49:"Insert",0x4A:"Home",0x4B:"PageUp",0x4C:"Delete",0x4D:"End",0x4E:"PageDown",0x4F:"Right",0x50:"Left",0x51:"Down",0x52:"Up",0x53:"NumLock",0x54:"Num/",0x55:"Num*",0x56:"Num-",0x57:"Num+",0x58:"NumEnter",0x59:"Num1",0x5A:"Num2",0x5B:"Num3",0x5C:"Num4",0x5D:"Num5",0x5E:"Num6",0x5F:"Num7",0x60:"Num8",0x61:"Num9",0x62:"Num0",0x63:"Num."};
// Phase 4: CSRF token injected by server at serve time
const CSRF_TOKEN=/*{{CSRF_TOKEN}}*/"";
let LAST_REVISION=/*{{STATE_REVISION}}*/"0";
function keyName(u){return HID_NAMES[u]||("?0x"+u.toString(16));}
// Reverse table: key name (lowercase) → usage ID. Users can type q/w/f1/space.
const NAME_TO_USAGE=(()=>{const m={};for(const u in HID_NAMES){m[HID_NAMES[u].toLowerCase()]=parseInt(u);}return m;})();
function resolveKey(input){input=input.trim().toLowerCase();if(!input)return 0;if(input.startsWith('0x')){const u=parseInt(input,16);return u>0?u:0;}if(NAME_TO_USAGE[input])return NAME_TO_USAGE[input];const u=parseInt(input,10);return u>0?u:0;}
// --- Action picker: tabs + chips ---
let activeActionTab='keys';
const CHIPS_KEYS=['{F1}','{F2}','{F3}','{F4}','{F5}','{F6}','{F7}','{F8}','{F9}','{F10}','{F11}','{F12}','{Space}','{Enter}','{Esc}','{Tab}','{Backspace}','{Left}','{Right}','{Up}','{Down}','{Home}','{End}','{PageUp}','{PageDown}','{Insert}','{Delete}','1','2','3','4','5','6','7','8','9','0','q','w','e','r','t','y','u','i','o','p','a','s','d','f','g','h','j','k','l','z','x','c','v','b','n','m'];
const CHIPS_MEDIA=['{Media_Play_Pause}','{Media_Next_Track}','{Media_Prev_Track}','{Media_Stop}','{Volume_Up}','{Volume_Down}','{Volume_Mute}'];
// AI-agent macro library: named combos/sequences (ported from Codex Micro/Stream Deck)
const MACROS=[
  {label:'Accept / Send',v:'{Enter}'},
  {label:'Cancel',v:'{Escape}'},
  {label:'Toggle sidebar',v:'{Ctrl+B}'},
  {label:'Voice / PTT',v:'{Ctrl+M}'},
  {label:'Send (Ctrl+Enter)',v:'{Ctrl+Enter}'},
  {label:'Branch session',v:'{Ctrl+Shift+F}'},
  {label:'Prompt history ↑',v:'{Up}'},
  {label:'Prompt history ↓',v:'{Down}'},
  {label:'Command mode ( / )',v:'{/}{Enter}'},
  {label:'Play / pause',v:'{Media_Play_Pause}'},
  {label:'Mute',v:'{Volume_Mute}'},
  {label:'Close app',v:'{Alt+F4}'}
];
function actionTab(name){activeActionTab=name;document.querySelectorAll('.action-tab').forEach(t=>{const active=t.dataset.tab===name;t.classList.toggle('active',active);t.setAttribute('aria-selected',active?'true':'false');});document.querySelectorAll('.action-panel').forEach(p=>p.classList.toggle('show',p.dataset.panel===name));}
function chipInsert(val,replace){const f=document.getElementById('newAction');if(!f)return;if(replace)f.value=val;else f.value=(f.value?f.value+' ':'')+val;updateMappingPreview();}
function chipSwitch(name){chipInsert('!switch:'+name,true);}
function chipToggle(name){chipInsert('!toggle:'+name,true);}
function chipLaunch(){openActionBuilder('Launch an application','!launch:','launch');}
function chipMulti(){openActionBuilder('Send keys to app','!app:','multi');}
function renderActionPicker(){let h='<div class="action-tabs" role="tablist" aria-label="Action type">';
h+='<button type="button" class="action-tab active" role="tab" aria-selected="true" data-tab="keys" onclick="actionTab(\'keys\')">Keys</button>';
h+='<button type="button" class="action-tab" role="tab" aria-selected="false" data-tab="media" onclick="actionTab(\'media\')">Media</button>';
h+='<button type="button" class="action-tab" role="tab" aria-selected="false" data-tab="switch" onclick="actionTab(\'switch\')">Switch profile</button>';
h+='<button type="button" class="action-tab" role="tab" aria-selected="false" data-tab="launch" onclick="actionTab(\'launch\')">Launch app</button>';
h+='<button type="button" class="action-tab" role="tab" aria-selected="false" data-tab="multi" onclick="actionTab(\'multi\')">Send to app</button>';
h+='<button type="button" class="action-tab" role="tab" aria-selected="false" data-tab="macros" onclick="actionTab(\'macros\')">Macros</button>';
h+='</div>';
h+='<div class="action-panel show" data-panel="keys">';for(const c of CHIPS_KEYS)h+='<button type="button" class="chip cat-keys" onclick="chipInsert(\''+c.replace(/'/g,'\\\\\'')+'\',true)">'+c+'</button>';h+='</div>';
h+='<div class="action-panel" data-panel="media">';for(const c of CHIPS_MEDIA)h+='<button type="button" class="chip cat-media" onclick="chipInsert(\''+c+'\',true)">'+c.replace(/[{}]/g,'')+'</button>';h+='</div>';
h+='<div class="action-panel" data-panel="switch">';
for(const p of state.profiles){if(sel&&p.name===sel.name)continue;h+='<button type="button" class="chip cat-switch" onclick="chipSwitch(\''+jsStr(p.name)+'\')">Switch to '+esc(p.name)+'</button> ';}
h+='<button type="button" class="chip cat-switch" onclick="chipToggle(\'basic\')">Toggle basic</button>';
h+='</div>';
h+='<div class="action-panel" data-panel="launch"><button type="button" class="chip cat-launch" onclick="chipLaunch()">Choose application…</button></div>';
h+='<div class="action-panel" data-panel="multi"><div id="multiAppBuilder">';
// Visual multi-app: dropdown to select target app + action sub-selector
h+='<div class="field-grid"><div class="field"><label for="multiAppTarget">Send to</label><select id="multiAppTarget" onchange="updateMappingPreview()">';
h+='<option value="">— Default (profile target) —</option>';
for(const app of state.apps||[]){h+='<option value="'+esc(app.name)+'">'+esc(app.name)+'</option>';}
h+='</select></div>';
h+='<div class="field"><label for="multiAppAction">Action</label><input id="multiAppAction" placeholder="{F1} or {Media_Next_Track}" oninput="updateMappingPreview()"></div></div>';
h+='<div style="display:flex;gap:8px;align-items:center;margin-top:8px">';
h+='<button type="button" class="btn" onclick="pickAppFromRunning()">Pick from running windows</button>';
h+='<span class="field-hint">Select app visually, then choose action</span></div>';
h+='</div></div>';
h+='<div class="action-panel" data-panel="macros"><p class="panel-hint">One-shot agent hotkeys (combos / sequences). Pick a target app profile, then tap.</p>';
for(const m of MACROS)h+='<button type="button" class="chip cat-switch" title="'+esc(m.v)+'" onclick="chipInsert(\''+m.v.replace(/'/g,"\\'")+'\',true)">'+esc(m.label)+'</button>';
h+='<div class="combo-row"><select id="comboMod"><option value="">— modifier —</option><option value="Ctrl">Ctrl</option><option value="Shift">Shift</option><option value="Alt">Alt</option><option value="Win">Win</option><option value="LCtrl">LCtrl</option><option value="RCtrl">RCtrl</option><option value="LShift">LShift</option><option value="RShift">RShift</option><option value="LAlt">LAlt</option><option value="RAlt">RAlt</option><option value="LWin">LWin</option><option value="RWin">RWin</option></select><input id="comboKey" placeholder="key (B / F5 / Enter)" style="width:170px"><button type="button" class="btn small" onclick="comboInsert()">Insert combo</button></div>';
h+='</div>';
h+='<p class="action-help">For letter hotkeys, choose one Latin key such as <code>m</code> or <code>e</code>. A label like <code>E</code> describes the physical key only.</p>';
return h;}
// ---- Typed Action Builder: modal instead of prompt() for chipLaunch/chipMulti/editMapping ----
let abState={onSave:null,mode:'general',editing:null};
function abInsert(val,replace){
  const f=document.getElementById('abAction');
  if(!f)return;
  if(replace)f.value=val;
  else f.value=(f.value?f.value+' ':'')+val;
  abPreview();
}
function abPreview(){
  const f=document.getElementById('abAction');
  const p=document.getElementById('abPreview');
  if(!f||!p)return;
  const v=f.value.trim();
  p.innerHTML=v?('→ '+esc(actionDescription(v))):'<span class="muted">Type an action or tap a chip…</span>';
}
function abTab(name){
  document.querySelectorAll('#abModal .action-tab').forEach(t=>{const active=t.dataset.tab===name;t.classList.toggle('active',active);t.setAttribute('aria-selected',active?'true':'false');});
  document.querySelectorAll('#abModal .action-panel').forEach(p=>p.classList.toggle('show',p.dataset.panel===name));
}
function abChips(){
  let h='<div class="action-tabs" role="tablist" aria-label="Action builder">';
  h+='<button type="button" class="action-tab active" role="tab" aria-selected="true" data-tab="keys" onclick="abTab(\'keys\')">Keys</button>';
  h+='<button type="button" class="action-tab" role="tab" aria-selected="false" data-tab="media" onclick="abTab(\'media\')">Media</button>';
  h+='<button type="button" class="action-tab" role="tab" aria-selected="false" data-tab="macros" onclick="abTab(\'macros\')">Macros</button>';
  h+='<button type="button" class="action-tab" role="tab" aria-selected="false" data-tab="switch" onclick="abTab(\'switch\')">Switch</button>';
  h+='<button type="button" class="action-tab" role="tab" aria-selected="false" data-tab="launch" onclick="abTab(\'launch\')">Launch</button>';
  h+='<button type="button" class="action-tab" role="tab" aria-selected="false" data-tab="multi" onclick="abTab(\'multi\')">Send to app</button>';
  h+='</div>';
  h+='<div class="action-panel show" data-panel="keys">';for(const c of CHIPS_KEYS)h+='<button type="button" class="chip cat-keys" onclick="abInsert(\''+c.replace(/'/g,'\\\'')+'\')">'+c+'</button>';h+='</div>';
  h+='<div class="action-panel" data-panel="media">';for(const c of CHIPS_MEDIA)h+='<button type="button" class="chip cat-media" onclick="abInsert(\''+c+'\')">'+c.replace(/[{}]/g,'')+'</button>';h+='</div>';
  h+='<div class="action-panel" data-panel="macros">';for(const m of MACROS)h+='<button type="button" class="chip cat-switch" title="'+esc(m.v)+'" onclick="abInsert(\''+m.v.replace(/'/g,"\\'")+'\')">'+esc(m.label)+'</button>';h+='<div class="combo-row"><select id="abComboMod"><option value="">— modifier —</option><option value="Ctrl">Ctrl</option><option value="Shift">Shift</option><option value="Alt">Alt</option><option value="Win">Win</option></select><input id="abComboKey" placeholder="key (B / F5 / Enter)" style="width:170px"><button type="button" class="btn small" onclick="const km=document.getElementById(\'abComboMod\').value,kk=document.getElementById(\'abComboKey\').value.trim();if(kk)abInsert(km?(\'{\'+km+\'+\'+kk.replace(/[{}]/g,\'\')+\'}\'):(\'{\'+kk.replace(/[{}]/g,\'\')+\'}\'))">Insert combo</button></div></div>';
  h+='<div class="action-panel" data-panel="switch">';for(const p of state.profiles){if(sel&&p.name===sel.name)continue;h+='<button type="button" class="chip cat-switch" onclick="abInsert(\'!switch:'+jsStr(p.name)+'\',true)">Switch to '+esc(p.name)+'</button> ';}
  h+='<button type="button" class="chip cat-switch" onclick="abInsert(\'!toggle:basic\',true)">Toggle basic</button></div>';
  h+='<div class="action-panel" data-panel="launch"><button type="button" class="chip cat-launch" onclick="abPickLaunch()">Pick running window…</button><span class="panel-hint">Picks the app exe path of a running window into !launch:</span></div>';
  h+='<div class="action-panel" data-panel="multi"><select id="abAppTarget" style="width:100%;margin-bottom:6px"><option value="">— pick app —</option></select><div style="display:flex;gap:6px"><button type="button" class="btn small" onclick="abPickAppList()">Load running apps</button><button type="button" class="btn small" onclick="abInsertApp()">Insert !app:</button></div></div>';
  return h;
}
async function abPickLaunch(){
  try{
    const fg=await api('GET','/api/v1/windows/foreground');
    if(fg.found&&fg.processPath){abInsert('!launch:'+fg.processPath,true);toast('Picked: '+fg.processPath,'success');}
    else toast('No foreground window with a path','error');
  }catch(e){toast('Pick failed: '+e.message,'error');}
}
async function abPickAppList(){
  const selEl=document.getElementById('abAppTarget');
  if(!selEl)return;
  try{
    const r=await api('GET','/api/v1/windows');
    selEl.innerHTML='<option value="">— pick app —</option>';
    const seen={};
    for(const w of (r.windows||[])){
      const name=w.processName||w.windowClass||'window';
      if(seen[name])continue;seen[name]=1;
      selEl.innerHTML+='<option value="'+esc(name)+'">'+esc(name)+'</option>';
    }
    toast('Loaded '+(r.windows||[]).length+' windows','success');
  }catch(e){toast('Load failed: '+e.message,'error');}
}
function abInsertApp(){
  const t=document.getElementById('abAppTarget');
  if(!t||!t.value){toast('Pick a target app first','error');return;}
  abInsert('!app:'+t.value+':{',false);
  const f=document.getElementById('abAction');
  if(f){f.focus();const pos=f.value.length;try{f.setSelectionRange(pos,pos+1);}catch(e){}}
}
function openActionBuilder(title,initial,mode){
  abState.mode=mode||'general';
  const m=document.getElementById('appModal');
  let h='<div class="modal-overlay" onclick="if(event.target===this)closeModal()"><div class="modal" role="dialog" aria-modal="true" aria-label="Action builder"><div class="modal-header"><h3>'+esc(title)+'</h3><button class="btn small" aria-label="Close" onclick="closeModal()">✕</button></div><div class="modal-body" id="abModal">';
  h+='<textarea id="abAction" class="ab-textarea" placeholder="Type an action: {F1}, {Ctrl+Shift+F}, !launch:C:\\path\\app.exe, !switch:profile…" oninput="abPreview()">'+esc(initial||'')+'</textarea>';
  h+='<div id="abPreview" class="ab-preview">'+((initial&&initial!=='!launch:'&&initial!=='!app:')?('→ '+esc(actionDescription(initial))):'<span class="muted">Type an action or tap a chip…</span>')+'</div>';
  h+=abChips();
  h+='<div class="ab-actions"><button class="btn" onclick="closeModal()">Cancel</button><button class="btn primary" onclick="abSave()">Apply</button></div>';
  h+='</div></div></div>';
  m.innerHTML=h;m.style.display='block';
  document.addEventListener('keydown',_modalKeyHandler);
  abState.onSave=null;
  const f=document.getElementById('abAction');
  if(f){f.focus();const pos=f.value.length;try{f.setSelectionRange(pos,pos);}catch(e){}}
}
function abSave(){
  const f=document.getElementById('abAction');
  if(!f)return;
  const v=f.value.trim();
  if(!v){toast('Action cannot be empty','error');return;}
  closeModal();
  if(abState.onSave)abState.onSave(v);
}
function modName(m){let s="";if(m&0x33)s+="Ctrl+";if(m&0x22)s+="Shift+";if(m&0x44)s+="Alt+";if(m&0x88)s+="Win+";return s.replace(/\+$/,"");}
let state={active:"",profiles:[],apps:[]};let sel=null;let selectedProfileName='';let currentView='profile';let profileDirty=false;
async function api(m,p,b){const o={method:m,headers:{'Content-Type':'application/json'}};if(CSRF_TOKEN)o.headers['X-KeySidekick-Token']=CSRF_TOKEN;if(b!==undefined)o.body=JSON.stringify(b);const r=await fetch(p,o);const text=await r.text();let data={};try{data=text?JSON.parse(text):{};}catch(error){throw new Error('Invalid response from '+p);}if(!r.ok){data.ok=false;if(!data.error)data.error='Request failed ('+r.status+')';}return data;}
function cloneProfile(profile){return profile?JSON.parse(JSON.stringify(profile)):null;}
function profileModeLabel(profile){return profile.mode==='basic'?'Types normally':'Controls an app';}
function setConnectionStatus(id,ok,text){const item=document.getElementById(id);if(!item)return;item.classList.toggle('is-ok',ok===true);item.classList.toggle('is-error',ok===false);const value=item.querySelector('strong');if(value)value.textContent=text;}
function updateStatus(status){setConnectionStatus('statusDevice',status.device==='connected',status.device==='connected'?'Connected':'Disconnected');const active=document.getElementById('statusActiveText');if(active)active.textContent=status.active||state.active||'—';}
async function refreshActiveTargetStatus(){const profile=state.profiles.find(p=>p.name===state.active);if(!profile||profile.mode==='basic'){setConnectionStatus('statusTarget',true,'Not used in basic');return;}setConnectionStatus('statusTarget',null,'Checking…');const activeName=profile.name;try{const result=await api('POST','/api/v1/applications/test-resolve',{windowClass:profile.targetClass,processName:profile.targetExe,processPath:profile.targetPath});if(state.active!==activeName)return;setConnectionStatus('statusTarget',!!result.found,result.found?'Available':'Not found');}catch(error){if(state.active===activeName)setConnectionStatus('statusTarget',false,'Check failed');}}
function renderLoadError(error){document.getElementById('editor').innerHTML='<div class="empty-card"><span class="eyebrow">Dashboard unavailable</span><h2>Profiles could not be loaded</h2><p>'+esc(error&&error.message?error.message:'KeySidekick did not return profile data.')+'</p><button type="button" class="btn primary" onclick="refresh()">Retry</button></div>';}
async function refresh(){try{const status=await api('GET','/api/status');const profiles=await api('GET','/api/profiles');const appsResp=await api('GET','/api/v1/applications');state=profiles;state.apps=appsResp.applications||[];updateStatus(status);if(currentView!=='new'){let nextName=selectedProfileName;if(!state.profiles.some(p=>p.name===nextName))nextName=state.active;if(!state.profiles.some(p=>p.name===nextName)&&state.profiles.length)nextName=state.profiles[0].name;selectedProfileName=nextName||'';sel=cloneProfile(state.profiles.find(p=>p.name===selectedProfileName));profileDirty=false;}renderSidebar();if(currentView==='profile'){if(!state.profiles.some(p=>!p.isBuiltin)&&!localStorage.getItem('ks_onboarded'))renderOnboarding();else renderEditor();}refreshActiveTargetStatus();}catch(error){renderLoadError(error);}}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
// Value for embedding in single-quoted JS strings inside double-quoted HTML
// attributes. Escape HTML metacharacters (& < ") first so the attribute parses
// intact, then backslash, quote, and newlines so the JS string parses.
function jsStr(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;').replace(/\\/g,'\\\\').replace(/'/g,"\\'").replace(/\r/g,'\\r').replace(/\n/g,'\\n');}
function selectProfile(name){stopAllPolling();const profile=state.profiles.find(p=>p.name===name);if(!profile)return;selectedProfileName=name;sel=cloneProfile(profile);currentView='profile';profileDirty=false;renderSidebar();renderEditor();}
function renderSidebar(){const l=document.getElementById('profileList');const count=document.getElementById('profileCount');if(count)count.textContent=state.profiles.length;l.innerHTML='';for(const p of state.profiles){const button=document.createElement('button');button.type='button';button.className='profile-item'+(p.name===state.active?' is-active':'')+(currentView==='profile'&&p.name===selectedProfileName?' is-selected':'');button.innerHTML='<span class="profile-copy"><strong>'+esc(p.name)+'</strong><small>'+esc(profileModeLabel(p))+' · '+p.keys.length+' mapping'+(p.keys.length===1?'':'s')+'</small></span>'+(p.name===state.active?'<span class="active-badge">Active</span>':'');button.onclick=()=>selectProfile(p.name);l.appendChild(button);}}
function markProfileDirty(){profileDirty=true;const save=document.getElementById('saveProfileBtn');if(save)save.disabled=false;const status=document.getElementById('saveState');if(status)status.textContent='Unsaved changes';}
function toggleTargetSettings(mode){const target=document.getElementById('targetSettings');if(target)target.hidden=mode!=='targeted';markProfileDirty();}
function actionDescription(action){if(action.startsWith('!switch:'))return'Switch to '+action.slice(8);if(action.startsWith('!toggle:'))return'Toggle '+action.slice(8);if(action.startsWith('!launch:'))return'Launch '+action.slice(8).split(/[\\/]/).pop();if(action.startsWith('!app:')){const rest=action.slice(5);const split=rest.indexOf(':');return split>0?rest.slice(split+1)+' → '+rest.slice(0,split):action;}return describeMacro(action);}
function describeMacro(a){const s=String(a||'');const out=s.replace(/\{([^}]*)\}/g,function(m,g){return '['+g.trim().replace(/\+/g,' + ')+']';}).trim();return out||s;}
function mappingDescription(profile,mapping){let description=actionDescription(mapping.description||mapping.action);const target=profile.targetExe||profile.targetClass;if(target&&profile.mode==='targeted'&&(mapping.category==='key'||mapping.category==='media'))description+=' → '+target;return description;}
function ambiguousKeyAction(action){return /^[A-Za-z]\s*\/\s*/.test(action);}
function updateMappingPreview(){
  const preview=document.getElementById('mappingPreview');
  if(!preview)return;
  const key=document.getElementById('newKeyInput');
  const keyText=key&&key.value.trim()?key.value.trim():'Choose a key';
  // Visual multi-app builder takes priority if filled
  const mt=document.getElementById('multiAppTarget');
  const ma=document.getElementById('multiAppAction');
  if(mt&&ma&&ma.value.trim()){
    const target=mt.value||'(profile target)';
    preview.textContent=keyText+' → '+actionDescription(ma.value.trim())+' → '+target;
    return;
  }
  const action=document.getElementById('newAction');
  const actionText=action&&action.value.trim()?actionDescription(action.value.trim()):'choose an action';
  preview.textContent=keyText+' → '+actionText;
}
function renderEditor(){if(!sel){document.getElementById('editor').innerHTML='<div class="empty-card"><span class="eyebrow">No profiles</span><h2>Create your first keyboard mode</h2><p>Choose whether this keyboard should type normally or control one application.</p><button type="button" class="btn primary" onclick="newProfile()">Create profile</button></div>';return;}const p=sel;const ib=p.isBuiltin;const active=p.name===state.active&&!p.isNew;const targetLabel=p.targetExe||p.targetClass||'No application selected';let h='<div class="profile-page">';
h+='<header class="profile-header"><div><span class="eyebrow">'+(p.isNew?'New profile':'Editing profile')+'</span><div class="title-line"><h2>'+esc(p.isNew?'Create profile':p.name)+'</h2>'+(active?'<span class="active-badge large">Active now</span>':'')+'</div><p>'+esc(profileModeLabel(p))+' · '+p.keys.length+' mapping'+(p.keys.length===1?'':'s')+'</p></div><div class="profile-actions">';
if(p.isNew)h+='<button type="button" class="btn primary" onclick="saveNewProfile()">Create profile</button>';else if(!ib)h+='<button type="button" class="btn primary" id="saveProfileBtn" onclick="saveProfile()" '+(profileDirty?'':'disabled')+'>Save changes</button>';if(!active&&!p.isNew)h+='<button type="button" class="btn" onclick="activate()">Activate profile</button>';h+='</div></header>';
if(!p.isNew&&!ib)h+='<div class="secondary-actions"><span id="saveState">'+(profileDirty?'Unsaved changes':'All profile settings saved')+'</span><button type="button" class="btn quiet" onclick="renameProfile()">Rename</button><button type="button" class="btn quiet" onclick="duplicateProfile()">Duplicate</button><button type="button" class="btn quiet danger" onclick="deleteProfile()">Delete</button></div>';
h+='<section class="content-card"><div class="card-heading"><div><span class="step-number">1</span><div><h3>Profile overview</h3><p>Choose where this profile sends the dedicated keyboard.</p></div></div></div><div class="field-grid"><div class="field"><label for="fName">Profile name</label><input id="fName" value="'+esc(p.name)+'" '+(ib?'readonly':'oninput="markProfileDirty()"')+'></div><div class="field"><label for="fMode">How it works</label><select id="fMode" '+(ib?'disabled':'onchange="toggleTargetSettings(this.value)"')+'><option value="basic" '+(p.mode==='basic'?'selected':'')+'>Types normally in the focused app</option><option value="targeted" '+(p.mode==='targeted'?'selected':'')+'>Controls one app in the background</option></select></div><div class="field"><label for="fLayerMod">Fn layer</label><select id="fLayerMod" '+(ib?'disabled':'onchange="markProfileDirty()"')+'><option value="">Off — no Fn layer</option><option value="Alt" '+(p.layerMod==='Alt'?'selected':'')+'>Alt</option><option value="Ctrl" '+(p.layerMod==='Ctrl'?'selected':'')+'>Ctrl</option><option value="Shift" '+(p.layerMod==='Shift'?'selected':'')+'>Shift</option><option value="Win" '+(p.layerMod==='Win'?'selected':'')+'>Win</option><option value="LCtrl" '+(p.layerMod==='LCtrl'?'selected':'')+'>Left Ctrl</option><option value="RCtrl" '+(p.layerMod==='RCtrl'?'selected':'')+'>Right Ctrl</option><option value="LShift" '+(p.layerMod==='LShift'?'selected':'')+'>Left Shift</option><option value="RShift" '+(p.layerMod==='RShift'?'selected':'')+'>Right Shift</option><option value="LAlt" '+(p.layerMod==='LAlt'?'selected':'')+'>Left Alt</option><option value="RAlt" '+(p.layerMod==='RAlt'?'selected':'')+'>Right Alt</option><option value="LWin" '+(p.layerMod==='LWin'?'selected':'')+'>Left Win</option><option value="RWin" '+(p.layerMod==='RWin'?'selected':'')+'>Right Win</option></select></div></div>';
if(ib){h+='<div class="info-callout"><strong>Basic is the safe fallback.</strong><span>Unmapped keys behave like a normal foreground keyboard.</span></div>';}else{h+='<div id="targetSettings" '+(p.mode==='targeted'?'':'hidden')+'><div class="target-summary"><div><span class="eyebrow">Target application</span><strong id="targetSummaryText">'+esc(targetLabel)+'</strong><small>Keys keep going here even while another app is focused.</small></div><div><button type="button" class="btn" onclick="pickFromWindow()">Choose running window</button><button type="button" class="btn" onclick="pickForeground()">Pick foreground app</button><button type="button" class="btn" onclick="testResolve()">Test connection</button></div></div><details class="advanced"><summary>Advanced target settings</summary><div class="advanced-body"><div class="field-grid"><div class="field"><label for="fClass">Window class</label><input id="fClass" value="'+esc(p.targetClass)+'" placeholder="TAIMPMainForm" oninput="markProfileDirty()"></div><div class="field"><label for="fExe">Process name</label><input id="fExe" value="'+esc(p.targetExe)+'" placeholder="AIMP.exe" oninput="markProfileDirty()"></div></div><div class="field"><label for="fPath">Executable path</label><input id="fPath" value="'+esc(p.targetPath)+'" placeholder="C:\\\\...\\\\app.exe" oninput="markProfileDirty()"></div><label class="check-field"><input type="checkbox" id="fAuto" '+(p.autoStart?'checked':'')+' onchange="markProfileDirty()"> Start the app when its window is missing</label>';
if(p.linkedApplications&&p.linkedApplications.length){h+='<div class="linked-apps"><span>Linked application IDs</span><div class="app-list">';for(const appId of p.linkedApplications){const isDefault=appId===p.defaultApplication;h+='<span class="app-badge'+(isDefault?' default':'')+'">'+esc(appId)+(isDefault?' ★':'')+'</span>';}h+='</div></div>';}
h+='</div></details></div>';}h+='</section>';
  if(p.isNew){h+='<div class="info-callout"><strong>Create the profile first.</strong><span>Then capture keys and add mappings.</span></div>';}else{h+='<section class="content-card"><div class="card-heading"><div><span class="step-number">2</span><div><h3>Key mappings</h3><p>What each physical key does in this profile.</p></div></div><span class="count-badge">'+p.keys.length+'</span></div><div class="mapping-list">';if(!p.keys.length)h+='<div class="mini-empty">No mappings yet. Add the first one below.</div>';for(const k of p.keys){const category=k.category||'key';const ambiguous=ambiguousKeyAction(k.action);h+='<article class="mapping-card'+(ambiguous?' has-warning':'')+'"><div class="mapping-trigger"><span class="key-badge">'+esc(keyName(k.usage))+'</span><small>'+(k.mod?'<span class="layer-badge">Fn · '+esc(modName(k.mod))+'</span>':'<span class="layer-badge plain">base</span>')+'</small></div><div class="mapping-action"><span class="category-badge cat-'+esc(category)+'">'+esc(category)+'</span><strong>'+esc(mappingDescription(p,k))+'</strong><code>'+esc(k.action)+'</code>'+(ambiguous?'<small class="mapping-warning">Use <code>'+esc(k.action.charAt(0).toLowerCase())+'</code> as the Action for reliable hold.</small>':'')+'</div><div class="mapping-btns"><button type="button" class="icon-btn" title="Move up" onclick="moveKeyUp('+k.usage+','+k.mod+')">↑</button><button type="button" class="icon-btn" title="Move down" onclick="moveKeyDown('+k.usage+','+k.mod+')">↓</button><button type="button" class="icon-btn" title="Edit action" onclick="editMapping('+k.usage+','+k.mod+',\''+esc(k.action).replace(/'/g,"\\'")+'\')">✎</button><button type="button" class="icon-btn danger" aria-label="Remove mapping for '+esc(keyName(k.usage))+'" onclick="delKey('+k.usage+','+k.mod+')">×</button></div></article>';}h+='</div></section>';
h+='<section class="content-card mapping-builder"><div class="card-heading"><div><span class="step-number">3</span><div><h3>Add a mapping</h3><p>Press a key, choose an action, review, then add.</p></div></div></div><div class="builder-step"><div class="builder-label"><span>1</span><strong>Press a key</strong></div><div class="capture-row"><button type="button" class="btn primary" id="captureBtn" onclick="captureKey()">Capture from keyboard</button><span id="captureHint">or enter it manually</span></div><div class="field-grid compact"><div class="field"><label for="newKeyInput">Physical key</label><input id="newKeyInput" placeholder="q / f1 / space" oninput="delete this.dataset.usage;updateMappingPreview()"></div><div class="field"><label for="newMod">Fn layer / while holding</label><select id="newMod" onchange="updateMappingPreview()"><option value="0">base — no modifier</option><option value="51">Fn · Ctrl + Shift</option><option value="17">Fn · Ctrl</option><option value="34">Fn · Shift</option><option value="68">Fn · Alt</option><option value="85">Fn · Ctrl + Alt</option></select></div></div></div><div class="builder-step"><div class="builder-label"><span>2</span><strong>Choose what it does</strong></div>'+renderActionPicker()+'<details class="advanced manual-action"><summary>Advanced / enter action manually</summary><div class="advanced-body"><div class="field"><label for="newAction">Raw action</label><input id="newAction" placeholder="{F1} or !switch:basic" oninput="updateMappingPreview()"></div></div></details></div><div class="builder-step review-step"><div class="builder-label"><span>3</span><strong>Review and add</strong></div><div class="mapping-preview" id="mappingPreview">Choose a key → choose an action</div><button type="button" class="btn primary" onclick="addKey()">Add mapping</button></div></section>';}
h+='<section class="content-card"><div class="card-heading"><div><span class="step-number">4</span><div><h3>Active keyboard — which pad this profile reads</h3><p>One keyboard is active. Change it in Keyboard Setup.</p></div></div></div><div id="activeKbdCard">Loading…</div><div style="margin-top:8px"><button type="button" class="btn" onclick="showDevices()">Open keyboard setup</button></div></section>';
h+='<section class="content-card"><div class="card-heading"><div><span class="step-number">5</span><div><h3>Extras — tap to try</h3><p>Media, multi-app, combos, launch, profile switching — all from one keyboard.</p></div></div></div>'+featureGridHtml()+'</section>';
h+='</div>';
// Focus management: preserve focus across SSE-driven re-renders
const _focusedId=document.activeElement?document.activeElement.id:null;
const _selStart=_focusedId&&document.activeElement?document.activeElement.selectionStart:null;
const _selEnd=_focusedId&&document.activeElement?document.activeElement.selectionEnd:null;
document.getElementById('editor').innerHTML=h;activeActionTab='keys';updateMappingPreview();loadActiveKbd();
// Restore focus if the same element still exists
if(_focusedId){const el=document.getElementById(_focusedId);if(el){try{el.focus();if(_selStart!==null&&_selEnd!==null&&el.setSelectionRange)el.setSelectionRange(_selStart,_selEnd);}catch(e){}}}}
// ---- First-run onboarding: empty dashboard → path choice ----
async function renderOnboarding(){
  stopAllPolling();
  document.getElementById('editor').innerHTML='<div class="empty-card onboard-card" style="max-width:720px">'
  +'<span class="eyebrow">Welcome to KeySidekick</span>'
  +'<h2>Turn the spare keyboard into a control pad</h2>'
  +'<p>Pick how to start. You can change everything later — profiles, keys and targets stay editable.</p>'
  +'<div id="onboardDeviceNote"></div>'
  +'<div class="onboard-grid">'
  +'<div class="onboard-option" onclick="showPresetWizard()"><div class="onboard-option-icon">📦</div><h3>Pad template</h3><p>Instant profile: AI agents, media, OBS, DAW, video, meetings, PowerPoint.</p><span class="btn primary small" style="pointer-events:none">Browse templates</span></div>'
  +'<div class="onboard-option" onclick="newProfile()"><div class="onboard-option-icon">🛠️</div><h3>Create a profile</h3><p>Typing or app-control profile, then map keys one by one.</p><span class="btn small" style="pointer-events:none">Build manually</span></div>'
  +'<div class="onboard-option" onclick="showDevices()"><div class="onboard-option-icon">⌨️</div><h3>Keyboard setup</h3><p>The keyboard must be on the WinUSB driver before keys are captured.</p><span class="btn small" style="pointer-events:none">Check devices</span></div>'
  +'</div>'
  +'<p style="margin-top:18px"><button class="btn quiet" onclick="localStorage.setItem(\'ks_onboarded\',\'1\');renderEditor();">Skip — show built-in profiles</button></p>'
  +'</div>';
  try{
    const st=await api('GET','/api/v1/state');
    const active=st.active||state.active||'';
    if(st.device!=='connected'&&!active){
      const note=document.getElementById('onboardDeviceNote');
      if(note)note.innerHTML='<div class="info-callout" style="margin-top:12px"><strong>No keyboard active</strong><span> — use "+ Setup keyboard" to connect yours.</span><button class="btn small primary" style="margin-left:auto" onclick="showWizard()">+ Setup keyboard</button></div>';
    }
  }catch(e){}
}
async function saveProfile(){if(!sel)return;const ib=sel.isBuiltin;const body={name:document.getElementById('fName').value,mode:document.getElementById('fMode').value,targetClass:ib?'':document.getElementById('fClass').value,targetExe:ib?'':document.getElementById('fExe').value,targetPath:ib?'':document.getElementById('fPath').value,autoStart:ib?false:document.getElementById('fAuto').checked,layerMod:ib?'':document.getElementById('fLayerMod').value};const r=await api('POST','/api/profile',body);if(r.ok){profileDirty=false;toast('Profile settings saved','success');await refresh();}else toast(r.error||'Save failed','error');}
async function activate(){const name=sel.name;const r=await api('POST','/api/profile/activate',{name:name});if(r.ok){toast('Active profile: '+name,'success');await refresh();}else toast(r.error||'Activation failed','error');}
async function delKey(u,m){const r=await api('POST','/api/key/delete',{profile:sel.name,usage:u,mod:m});if(r.ok){toast('Mapping removed','success');await refresh();}else toast(r.error||'Remove failed','error');}
async function moveKeyUp(u,m){const r=await api('POST','/api/key/move',{profile:sel.name,usage:u,mod:m,direction:'up'});if(r.ok){await refresh();}else toast(r.error||'Move failed','error');}
async function moveKeyDown(u,m){const r=await api('POST','/api/key/move',{profile:sel.name,usage:u,mod:m,direction:'down'});if(r.ok){await refresh();}else toast(r.error||'Move failed','error');}
async function duplicateKey(u,m){const newKey=prompt('Copy to key (e.g. q, f2, space):');if(!newKey)return;const usage=resolveKey(newKey);if(!usage){toast('Unknown key','error');return;}const r=await api('POST','/api/key/duplicate',{profile:sel.name,usage:u,newUsage:usage});if(r.ok){toast('Copied to '+newKey,'success');await refresh();}else toast(r.error||'Duplicate failed','error');}
async function editMapping(u,m,currentAction){openActionBuilder('Edit action for '+keyName(u),currentAction||'');abState.onSave=async(newAction)=>{const r=await api('POST','/api/key/update',{profile:sel.name,usage:u,mod:m,newAction:newAction});if(r.ok){toast('Updated: '+keyName(u),'success');await refresh();}else toast(r.error||'Update failed','error');};}
async function addKey(){
  const ki=document.getElementById('newKeyInput');
  if(!ki){toast('No key input','error');return;}
  const captured=parseInt(ki.dataset.usage||'0',10);
  const u=captured||resolveKey(ki.value);
  const ms=document.getElementById('newMod').value;
  const m=ms.startsWith('0x')?parseInt(ms,16):parseInt(ms,10);
  if(!u){toast('Choose a key or capture it from the dedicated keyboard.','error');return;}
  // Visual multi-app tab takes priority if filled
  let a='';
  const multiTarget=document.getElementById('multiAppTarget');
  const multiAction=document.getElementById('multiAppAction');
  if(multiAction&&multiAction.value.trim()){
    const target=multiTarget?multiTarget.value:'';
    const action=multiAction.value.trim();
    if(target){a='!app:'+target+':'+action;}
    else{a=action;}
    toast('Multi-app: '+keyName(u)+' → '+action+' → '+target,'success');
  }else{
    const actionField=document.getElementById('newAction');
    a=actionField?actionField.value.trim():'';
  }
  if(!a){toast('Choose what the key should do.','error');return;}
  const r=await api('POST','/api/key',{profile:sel.name,usage:u,mod:m,action:a});
  if(r.ok){toast('Added: '+keyName(u)+' → '+actionDescription(a),'success');await refresh();}
  else toast(r.error||'Failed','error');
}
let ct=null;
async function captureKey(){
  // Guard against double-click — clear any existing capture session
  if(ct){clearInterval(ct);ct=null;}
  const b=document.getElementById('captureBtn');
  const h=document.getElementById('captureHint');
  if(!b||!h)return;
  try{
    await api('POST','/api/capture/start');
    b.classList.add('capture-active');
    b.textContent='Listening… press a key';
    h.textContent='Waiting for the dedicated keyboard';
    ct=setInterval(async()=>{
      try{
        const r=await api('GET','/api/capture/poll');
        if(r.ready){
          clearInterval(ct);ct=null;
          b.classList.remove('capture-active');
          b.textContent='Capture from keyboard';
          h.textContent='Key captured';
          const ki=document.getElementById('newKeyInput');
          if(ki){ki.value=keyName(r.usage);ki.dataset.usage=String(r.usage);}
          updateMappingPreview();
          toast('Captured: '+keyName(r.usage),'success');
        }
      }catch(e){
        // Poll failed — stop capture, don't leave button stuck
        clearInterval(ct);ct=null;
        b.classList.remove('capture-active');
        b.textContent='Capture from keyboard';
        h.textContent='Capture failed';
        toast('Capture failed: '+(e.message||'network error'),'error');
      }
    },100);
  }catch(e){
    // Start failed — server down or CSRF issue
    b.classList.remove('capture-active');
    b.textContent='Capture from keyboard';
    toast('Cannot start capture: '+(e.message||'server error'),'error');
  }
}
// ---- Create-profile wizard: steps (preset → keyboard → extras) ----
let createState={step:1,preset:null,kbd:''};
function newProfile(){stopAllPolling();selectedProfileName='';currentView='new';profileDirty=true;sel={name:'',mode:'targeted',targetClass:'',targetExe:'',targetPath:'',autoStart:false,isBuiltin:false,keys:[],linkedApplications:[],isNew:true};createState={step:1,preset:null,kbd:''};renderSidebar();renderCreateWizard();}
function renderCreateWizard(){
  const h='<div class="profile-page"><header class="profile-header"><div><span class="eyebrow">New profile</span><div class="title-line"><h2>Create profile</h2></div><p>Three steps — template, keyboard, extras. Everything can be changed later.</p></div><div class="profile-actions"><button type="button" class="btn primary" onclick="saveNewProfile()" id="createFinalBtn">Create profile</button></div></header>'
  +'<div class="create-steps">'
  +'<div class="create-step'+(createState.step===1?' on':'')+'" onclick="createStep(1)"><span class="n">Step 1</span><b>Start from a template</b></div>'
  +'<div class="create-step'+(createState.step===2?' on':'')+'" onclick="createStep(2)"><span class="n">Step 2</span><b>Keyboard and target</b></div>'
  +'<div class="create-step'+(createState.step===3?' on':'')+'" onclick="createStep(3)"><span class="n">Step 3</span><b>Extras</b></div>'
  +'</div>'
  // Step 1: name + mode + starting preset
  +'<div class="create-panel'+(createState.step===1?' show':'')+'" id="cp1">'
  +'<section class="content-card"><div class="card-heading"><div><span class="step-number">1</span><div><h3>Name and mode</h3><p>Basic types in the active window; targeted controls an app in the background.</p></div></div></div>'
  +'<div class="field-grid"><div class="field"><label for="fName">Profile name</label><input id="fName" value="'+esc(sel.name)+'" oninput="markProfileDirty()" placeholder="e.g. Media pad"></div>'
  +'<div class="field"><label for="fMode">How it works</label><select id="fMode" onchange="toggleTargetSettings(this.value)"><option value="basic" '+(sel.mode==='basic'?'selected':'')+'>Types in the active window</option><option value="targeted" '+(sel.mode==='targeted'?'selected':'')+'>Controls an app in the background</option></select></div></div></section>'
  +'<section class="content-card"><div class="card-heading"><div><span class="step-number">2</span><div><h3>Start from a pad template</h3><p>Optional: agent / media / DAW — F-keys are mapped automatically.</p></div></div></div>'
  +'<div id="createPresets" class="feature-grid">Loading templates…</div>'
  +'<p style="margin-top:10px" class="panel-hint">No template needed? Name the profile in step 1 and continue to step 2.</p></section>'
  +'</div>'
  // Step 2: keyboard + target
  +'<div class="create-panel'+(createState.step===2?' show':'')+'" id="cp2">'
  +'<section class="content-card"><div class="card-heading"><div><span class="step-number">1</span><div><h3>Which keyboard controls this profile</h3><p>The dedicated keyboard must be on the WinUSB driver (see Keyboard Setup). Choose which one sidekick reads.</p></div></div></div>'
  +'<div id="createKbdList">Loading keyboards…</div>'
  +'<div style="margin-top:8px"><button type="button" class="btn" onclick="showDevices()">Open keyboard setup</button></div></section>'
  +'<section class="content-card"><div class="card-heading"><div><span class="step-number">2</span><div><h3>Target application'+(sel.mode==='targeted'?'':' (basic — skip)')+'</h3><p>Keys go to this app even when focus is elsewhere.</p></div></div></div>'
  +'<div id="createTarget">'+(sel.mode==='targeted'?targetSettingsHtml():'<div class="info-callout">Basic mode: unmapped keys type normally, no target needed.</div>')+'</div></section>'
  +'</div>'
  // Step 3: extras
  +'<div class="create-panel'+(createState.step===3?' show':'')+'" id="cp3">'
  +'<section class="content-card"><div class="card-heading"><div><span class="step-number">1</span><div><h3>Extras — tap to try</h3><p>Everything works from the same keyboard: no extra hardware needed.</p></div></div></div>'
  +featureGridHtml()
  +'<p class="panel-hint" style="margin-top:10px">After creating the profile, add mappings: press a key, choose an action, review, add.</p></section>'
  +'</div>'
  +'<div class="create-nav"><button type="button" class="btn" onclick="createStep('+(createState.step>1?createState.step-1:1)+')" '+(createState.step===1?'disabled':'')+'>← Back</button>'
  +'<span class="panel-hint">Step '+createState.step+' of 3</span>'
  +'<button type="button" class="btn primary" onclick="createStep('+(createState.step<3?createState.step+1:3)+')">Next →</button></div>'
  +'</div>';
  const editor=document.getElementById('editor');
  editor.innerHTML=h;
  loadCreatePresets();
  loadCreateKbds();
}
function createStep(n){createState.step=Math.min(3,Math.max(1,n));const m=document.getElementById('fMode');if(m)sel.mode=m.value;const nm=document.getElementById('fName');if(nm)sel.name=nm.value;const cl=document.getElementById('fClass');if(cl)sel.targetClass=cl.value;const ex=document.getElementById('fExe');if(ex)sel.targetExe=ex.value;const pth=document.getElementById('fPath');if(pth)sel.targetPath=pth.value;const au=document.getElementById('fAuto');if(au)sel.autoStart=au.checked;renderCreateWizard();}
function targetSettingsHtml(){
  const p=sel;
  const targetLabel=p.targetExe||p.targetClass||'No application selected';
  return '<div id="targetSettings"><div class="target-summary"><div><span class="eyebrow">Target application</span><strong id="targetSummaryText">'+esc(targetLabel)+'</strong><small>Keys go here even when focus is elsewhere.</small></div><div><button type="button" class="btn" onclick="pickFromWindow()">Pick from running</button><button type="button" class="btn" onclick="pickForeground()">Use active window</button><button type="button" class="btn" onclick="testResolve()">Test connection</button></div></div>'
  +'<details class="advanced"><summary>Advanced target settings</summary><div class="advanced-body"><div class="field-grid"><div class="field"><label for="fClass">Window class</label><input id="fClass" value="'+esc(p.targetClass)+'" placeholder="TAIMPMainForm"></div><div class="field"><label for="fExe">Process name</label><input id="fExe" value="'+esc(p.targetExe)+'" placeholder="AIMP.exe"></div></div><div class="field"><label for="fPath">Path to exe</label><input id="fPath" value="'+esc(p.targetPath)+'" placeholder="C:\\...\\app.exe"></div><label class="check-field"><input type="checkbox" id="fAuto" '+(p.autoStart?'checked':'')+'> Launch the app when its window is not found</label></div></details>'
  +'<div class="linked-apps"><span>Multi-app — one keyboard, several programs</span>'
  +'<div class="app-list">'+(p.linkedApplications&&p.linkedApplications.length?p.linkedApplications.map(a=>'<span class="app-badge">'+esc(a)+'</span>').join(''):'<span class="panel-hint">Assign a "Send to app" action to a key and choose the program. Different keys — different apps.</span>')+'</div>'
  +'<button type="button" class="btn small app-add" onclick="openActionBuilder(\'Send to app\',\'!app:\',\'multi\')">+ Action for another app</button>'
  +'</div></div>';
}
function featureGridHtml(){
  const feats=[
    {t:'🎛 Media keys',d:'Play/pause, next track, volume — without touching the app window.',a:'openActionBuilder(\'Media key\',\'{Media_Play_Pause}\',\'general\')'},
    {t:'🧩 Multi-app',d:'One keyboard, several programs: different keys — different apps.',a:'openActionBuilder(\'Send to app\',\'!app:\',\'multi\')'},
    {t:'⚡ Combo hotkeys',d:'Ctrl+Shift+key for agents (accept / branch / sidebar).',a:'openActionBuilder(\'Combo hotkey\',\'{Ctrl+Shift+F}\',\'general\')'},
    {t:'🚀 Launch apps',d:'A key launches an app — by exe path or from a running window.',a:'chipLaunch()'},
    {t:'🔄 Switch profiles',d:'Keys switch the profile: typing ⇄ app control.',a:'openActionBuilder(\'Switch profile\',\'!switch:\',\'general\')'},
    {t:'🔥 Live fire',d:'Click a Live cell to fire the action right from the dashboard.',a:'showLive()'}
  ];
  return '<div class="feature-grid">'+feats.map(f=>'<div class="feature-card" onclick="'+f.a+'"><h4>'+f.t+'</h4><p>'+f.d+'</p><span class="feat-arrow">Try it →</span></div>').join('')+'</div>';
}
async function loadCreatePresets(){
  const el=document.getElementById('createPresets');
  let presets=[];
  try{const r=await api('GET','/api/v1/presets');presets=r.presets||[];}catch(e){}
  if(!presets||!presets.length){if(el)el.innerHTML='<span class="panel-hint">No templates.</span>';return;}
  window.__presets=presets;
  if(!el)return;
  el.innerHTML=presets.map(function(pst,i){
    const isSel=createState.preset&&createState.preset.agentId===pst.agentId;
    const legend=(pst.keys||[]).slice(0,6).map(k=>keyName(k.usage)+'→'+String(k.label||k.action).split('(')[0].trim()).join(' · ');
    return '<div class="feature-card'+(isSel?' sel':'')+'" onclick="pickPreset('+i+')"><h4>'+esc(pst.name)+'</h4><p>'+esc(pst.description||'')+(legend?'<br><code style="font-size:10px;color:var(--muted)">'+esc(legend)+'</code>':'')+'</p><span class="feat-arrow">'+(isSel?'Selected ✓':'Use template →')+'</span></div>';
  }).join('');
}
function pickPreset(i){
  const r=window.__presets||[];
  const pst=r[i];
  if(!pst)return;
  createState.preset=pst;
  const nameEl=document.getElementById('fName');
  if(nameEl&&!nameEl.value.trim())nameEl.value=pst.name+' pad';
  loadCreatePresets();
  toast('Template selected: '+pst.name,'success');
}
async function loadCreateKbds(){
  const el=document.getElementById('createKbdList');
  let devices=[];
  try{const r=await api('GET','/api/v1/hid');devices=r.devices||[];}catch(e){}
  const ready=devices.filter(d=>d.status==='ready');
  if(!el)return;
  if(!ready.length){
    el.innerHTML='<div class="kbd-card gray"><div class="kbd-keys"><span class="kbd-name">No WinUSB keyboard found</span><span class="kbd-path">Open Keyboard Setup to switch the driver with Zadig.</span></div></div>';
    return;
  }
  if(!createState.kbd)createState.kbd='vid_'+ready[0].vid+'&pid_'+ready[0].pid;
  el.innerHTML=ready.map(function(d){
    const vp='vid_'+d.vid+'&pid_'+d.pid;
    const selFlag=createState.kbd===vp;
    return '<label class="kbd-radio'+(selFlag?' sel':'')+'"><input type="radio" name="kbd" value="'+vp+'" '+(selFlag?'checked':'')+' onchange="pickKbd(\''+vp+'\')"><div class="kbd-keys"><span class="kbd-name">'+esc(d.name||'Keyboard')+'</span><span class="kbd-path">'+esc(vp.toUpperCase()+(d.mi?' [MI_'+d.mi+']':''))+'</span></div></label>';
  }).join('')
  +'<div class="app-add" style="margin-top:10px"><button type="button" class="btn primary" style="width:100%" onclick="activatePickedKbd()">Use this keyboard for the profile</button></div>';
}
function pickKbd(vp){createState.kbd=vp;}
async function loadActiveKbd(){
  const el=document.getElementById('activeKbdCard');
  if(!el)return;
  let diag=null,devices=[];
  try{diag=await api('GET','/api/v1/diagnostics');}catch(e){}
  try{const r=await api('GET','/api/v1/hid');devices=r.devices||[];}catch(e){}
  const activeVp=(diag&&diag.vidpid)||'';
  const activeId=activeVp.toLowerCase().replace(/^vid_/,'').replace(/&pid_/,'&');
  let found=null;
  for(const d of devices){
    if(('vid_'+d.vid+'&pid_'+d.pid).toLowerCase()===activeVp.toLowerCase())found=d;
  }
  if(found){
    el.innerHTML='<div class="kbd-card"><div class="kbd-keys"><span class="kbd-name">'+esc(found.name||'Keyboard')+'</span><span class="kbd-path">'+esc(activeVp.toUpperCase()+(found.mi?' [MI_'+found.mi+']':''))+' · WinUSB ready</span></div><span class="default-chip">ACTIVE</span></div>';
  }else if(activeVp){
    el.innerHTML='<div class="kbd-card gray"><div class="kbd-keys"><span class="kbd-name">'+esc(activeVp.toUpperCase())+'</span><span class="kbd-path">Active device — not in the WinUSB keyboard list</span></div></div>';
  }else{
    el.innerHTML='<div class="kbd-card gray"><div class="kbd-keys"><span class="kbd-name">No keyboard activated</span><span class="kbd-path">Open Keyboard Setup and activate the pad (WinUSB driver).</span></div></div>';
  }
}
async function activatePickedKbd(){
  if(!createState.kbd){toast('Pick a keyboard first','error');return;}
  const r=await api('POST','/api/v1/devices/activate',{vidpid:createState.kbd});
  if(r.ok){toast('Keyboard active: '+createState.kbd,'success');}
  else toast(r.error||'Activation failed','error');
}
async function deleteProfile(){
  if(!sel)return;
  if(sel.isBuiltin){toast('Cannot delete built-in profile','error');return;}
  if(!confirm('Delete profile "'+sel.name+'"? This cannot be undone.'))return;
  const r=await api('POST','/api/v1/profile/delete',{id:sel.name});
  if(r.ok){toast('Profile deleted','success');selectedProfileName='';currentView='profile';sel=null;await refresh();}
  else toast(r.error||'Delete failed','error');
}
async function renameProfile(){
  if(!sel||sel.isBuiltin){toast('Cannot rename built-in profile','error');return;}
  const newName=prompt('New profile name:',sel.name);
  if(!newName||newName===sel.name)return;
  const r=await api('POST','/api/v1/profile/rename',{id:sel.name,newName:newName});
  if(r.ok){toast('Renamed to '+newName,'success');selectedProfileName=newName;await refresh();}
  else toast(r.error||'Rename failed','error');
}
async function duplicateProfile(){
  if(!sel)return;
  const newName=prompt('Name for duplicated profile:',sel.name+' copy');
  if(!newName)return;
  const newId=newName.toLowerCase().replace(/[^a-z0-9_-]/g,'-').replace(/-+/g,'-').replace(/^-|-$/g,'');
  if(!newId){toast('Invalid name','error');return;}
  const r=await api('POST','/api/v1/profile/duplicate',{sourceId:sel.name,newId:newId,newName:newName});
  if(r.ok){toast('Profile duplicated as '+newName,'success');selectedProfileName=newName;await refresh();}
  else toast(r.error||'Duplicate failed','error');
}
async function saveNewProfile(){
  if(!sel||!sel.isNew)return;
  const name=document.getElementById('fName').value.trim();
  if(!name){toast('Name required','error');return;}
  const id=name.toLowerCase().replace(/[^a-z0-9_-]/g,'-').replace(/-+/g,'-').replace(/^-|-$/g,'');
  if(!id){toast('Invalid name (need letters/digits)','error');return;}
  // Template chosen in step 1 → create via preset/apply (mappings come from the catalog)
  if(createState.preset){
    const body={agentId:createState.preset.agentId,name:name};
    const cl=document.getElementById('fClass');if(cl&&cl.value.trim())body.windowClass=cl.value.trim();
    const ex=document.getElementById('fExe');if(ex&&ex.value.trim())body.processName=ex.value.trim();
    const pth=document.getElementById('fPath');if(pth&&pth.value.trim())body.exePath=pth.value.trim();
    const r=await api('POST','/api/v1/preset/apply',body);
    if(r.ok){toast('Profile created from template: '+name,'success');selectedProfileName=name;currentView='profile';await refresh();}
    else toast(r.error||'Create from template failed','error');
    return;
  }
  const mode=document.getElementById('fMode').value;
  const r=await api('POST','/api/v1/profile/create',{id:id,name:name,mode:mode});
  if(r.ok){
    toast('Profile created','success');
    // If targeted, set the target fields via existing SET_PROFILE
    if(mode==='targeted'){
      await api('POST','/api/profile',{name:name,mode:mode,
        targetClass:document.getElementById('fClass').value,
        targetExe:document.getElementById('fExe').value,
        targetPath:document.getElementById('fPath').value,
        autoStart:document.getElementById('fAuto').checked});
    }
    selectedProfileName=name;
    currentView='profile';
    await refresh();
  } else toast(r.error||'Create failed','error');
}
async function reloadConfig(){const r=await api('POST','/api/reload');if(r.ok){toast('Config reloaded','success');currentView='profile';await refresh();}else toast(r.error||'Reload failed','error');}
function reselect(){if(!selectedProfileName)return;const profile=state.profiles.find(p=>p.name===selectedProfileName);if(profile)sel=cloneProfile(profile);}
function toast(msg,type){const t=document.getElementById('toast');t.textContent=msg;t.className='toast show '+type;setTimeout(()=>{t.className='toast';},2500);}
// ---- Phase 3: Visual application picker ----
let _modalPreviousFocus=null;
async function pickFromWindow(){
  try{
    const r=await api('GET','/api/v1/windows');
    if(!r.windows||r.windows.length===0){toast('No windows found','error');return;}
    let h='<div class="modal-overlay" onclick="if(event.target===this)closeModal()"><div class="modal" role="dialog" aria-modal="true" aria-label="Pick running window"><div class="modal-header"><h3>Pick running window</h3><button class="btn small" aria-label="Close" onclick="closeModal()">✕</button></div><div class="modal-body"><p class="modal-hint">Select a window to use as target:</p>';
    for(const w of r.windows){
      h+='<div class="window-pick" tabindex="0" role="button" aria-label="'+esc(w.title)+' ('+esc(w.processName)+')" onclick="useWindow(\''+jsStr(w.windowClass)+'\',\''+jsStr(w.processName)+'\',\''+jsStr(w.processPath)+'\')" onkeydown="if(event.key===\'Enter\')useWindow(\''+jsStr(w.windowClass)+'\',\''+jsStr(w.processName)+'\',\''+jsStr(w.processPath)+'\')"><span class="window-title">'+esc(w.title)+'</span><span class="window-class">'+esc(w.windowClass)+'</span><span class="window-proc">'+esc(w.processName)+'</span></div>';
    }
    h+='</div></div></div>';
    const m=document.getElementById('appModal');
    m.innerHTML=h;
    m.style.display='block';
    // Focus management: save current focus, move into modal
    _modalPreviousFocus=document.activeElement;
    const firstPick=m.querySelector('.window-pick');
    if(firstPick)firstPick.focus();
    // Escape to close
    document.addEventListener('keydown',_modalKeyHandler);
  }catch(e){
    toast('Cannot load windows: '+(e.message||'error'),'error');
  }
}
function _modalKeyHandler(e){if(e.key==='Escape'){closeModal();}}
function closeModal(){
  const m=document.getElementById('appModal');
  if(m){m.style.display='none';m.innerHTML='';}
  document.removeEventListener('keydown',_modalKeyHandler);
  // Restore focus
  if(_modalPreviousFocus){try{_modalPreviousFocus.focus();}catch(e){}_modalPreviousFocus=null;}
}
function useWindow(cls,proc,path){closeModal();const fClass=document.getElementById('fClass');const fExe=document.getElementById('fExe');const fPath=document.getElementById('fPath');if(fClass)fClass.value=cls;if(fExe)fExe.value=proc;if(fPath)fPath.value=path;const summary=document.getElementById('targetSummaryText');if(summary)summary.textContent=proc||cls;markProfileDirty();toast('Target selected: '+(proc||cls),'success');}
async function testResolve(){if(!sel)return;const r=await api('POST','/api/v1/applications/test-resolve',{windowClass:document.getElementById('fClass').value,processName:document.getElementById('fExe').value,processPath:document.getElementById('fPath').value});if(r.found){toast('Window found (pid='+r.pid+')','success');}else{toast('Window not found','error');}}
// Pick app from running windows for multi-app target
async function pickAppFromRunning(){
  try{
    const r=await api('GET','/api/v1/windows');
    if(!r.windows||r.windows.length===0){toast('No running windows','error');return;}
    let h='<div class="modal-overlay" onclick="if(event.target===this)closeModal()"><div class="modal"><div class="modal-header"><h3>Choose target app</h3><button class="btn small" onclick="closeModal()">✕</button></div><div class="modal-body"><p class="modal-hint">Select a running window:</p>';
    for(const w of r.windows){
      h+='<div class="window-pick" tabindex="0" role="button" onclick="useAppWindow(\''+jsStr(w.processName)+'\',\''+jsStr(w.windowClass)+'\',\''+jsStr(w.processPath)+'\')" onkeydown="if(event.key===\'Enter\')useAppWindow(\''+jsStr(w.processName)+'\',\''+jsStr(w.windowClass)+'\',\''+jsStr(w.processPath)+'\')"><span class="window-title">'+esc(w.title)+'</span><span class="window-class">'+esc(w.windowClass)+'</span><span class="window-proc">'+esc(w.processName)+'</span></div>';
    }
    h+='</div></div></div>';
    const m=document.getElementById('appModal');
    m.innerHTML=h;m.style.display='block';
  }catch(e){toast('Cannot load windows: '+e.message,'error');}
}
function useAppWindow(proc,cls,path){
  closeModal();
  const sel=document.getElementById('multiAppTarget');
  if(!sel)return;
  // Add the app to the dropdown if not already there
  let found=false;
  for(let i=0;i<sel.options.length;i++){if(sel.options[i].value===proc){found=true;break;}}
  if(!found){
    const opt=document.createElement('option');
    opt.value=proc;opt.text=proc||cls;
    sel.appendChild(opt);
  }
  sel.value=proc;
  // Update multiAppAction preview
  updateMappingPreview();
  toast('Target: '+(proc||cls),'success');
}
// TinyWall-style: pick whatever window is currently in foreground
async function pickForeground(){
  toast('Switch to the app you want to control, then press OK','info');
  // Wait a moment for user to switch focus
  await new Promise(r=>setTimeout(r,2000));
  try{
    const r=await api('GET','/api/v1/windows/foreground');
    if(!r.found){toast('Could not detect foreground window','error');return;}
    const fClass=document.getElementById('fClass');
    const fExe=document.getElementById('fExe');
    const fPath=document.getElementById('fPath');
    if(fClass)fClass.value=r.windowClass||'';
    if(fExe)fExe.value=r.processName||'';
    if(fPath)fPath.value=r.processPath||'';
    const summary=document.getElementById('targetSummaryText');
    if(summary)summary.textContent=r.processName||r.windowClass||'Unknown';
    markProfileDirty();
    toast('Picked: '+(r.title||r.processName||r.windowClass),'success');
  }catch(e){toast('Pick failed: '+(e.message||'error'),'error');}
}
// ---- Device setup wizard ----
let lastDevices=[];               // last /api/v1/hid response (onclick by index)
let identifyPollTimer=null;
let wizardPollTimer=null;

function driverBadge(d){
  if(d.status==='ready')return '<span class="driver-badge ready">✓ WinUSB — ready</span>';
  if(d.status==='needs-driver')return '<span class="driver-badge warn">Normal keyboard (HidUsb) — needs driver swap</span>';
  return '<span class="driver-badge">'+esc(d.service||'HID')+'</span>';
}
function kindLabels(d){
  const m={keyboard:'⌨ Keyboard',mouse:'🖱 Mouse',consumer:'🎛 Media',generic:'HID',other:'Other',winusb:'WinUSB'};
  return String(d.kinds||'other').split(',').map(k=>m[k]||k).join(', ');
}
async function showDevices(){
  stopAllPolling();
  currentView='devices';
  const r=await api('GET','/api/v1/hid');
  lastDevices=r.devices||[];
  let st=null; try{ st=await api('GET','/api/v1/state'); }catch(e){}
  const ready=lastDevices.filter(d=>d.status==='ready');
  const ordinary=lastDevices.filter(d=>d.status!=='ready');
  let h='<div class="toolbar-row" style="display:flex;justify-content:space-between;align-items:center;margin-bottom:12px"><h2>Keyboard Setup</h2><button class="btn" onclick="showDevices()">Refresh</button></div>';
  if(st&&st.portChangeDetected){
    h+='<div class="info-callout warn" style="margin-bottom:14px"><strong>🔌 You changed the USB port.</strong>'
      +'<span>The keyboard '+esc(st.portChangeVidPid||'')+' is plugged into a different port — on the new port it is an ordinary keyboard again (Windows binds drivers per port). All your profiles are safe; one click restores everything:</span>'
      +'<button class="btn primary small" style="margin-left:auto;white-space:nowrap" onclick="applyDriverSwap(\''+esc(st.portChangeVidPid||'')+'\')">Apply driver again</button></div>';
  }
  h+='<div id="identifyFeed" class="identify-feed"></div>';
  if(!lastDevices.length){
    h+='<div class="info-callout"><strong>No input devices found.</strong><span>Connect your keyboard/mouse and make sure sidekick is running.</span></div>';
  }else{
    if(ready.length){
      h+='<h3>KeySidekick-ready keyboards</h3>';
      h+='<p class="modal-hint">These devices are on the WinUSB driver — KeySidekick reads them directly.</p>';
      h+='<div class="device-list">';
      for(let i=0;i<lastDevices.length;i++){
        const d=lastDevices[i];
        if(d.status!=='ready')continue;
        h+='<div class="device-card" data-vidpid="'+esc('vid_'+d.vid+'&pid_'+d.pid)+'">';
        h+='<div class="device-info">'+driverBadge(d)+'<span class="device-name">'+esc(d.name||'Unnamed device')+'</span><span class="device-path">'+esc('VID_'+d.vid+' PID_'+d.pid+(d.mi?' [MI_'+d.mi+']':''))+'</span></div>';
        h+='<div class="device-actions">';
        h+='<button class="btn small" onclick="identifyDeviceIdx('+i+')">Identify by keypress</button>';
        h+='<button class="btn small" onclick="testDeviceIdx('+i+')">Test</button>';
        h+='<button class="btn small" title="Add this exact instance by full device path (for two identical keyboards)" onclick="activateExact('+i+')">Activate exact</button>';
        h+='<button class="btn small" title="Make it a normal keyboard again (inbox HID driver)" onclick="restoreOriginalDriver(\'vid_'+d.vid+'&pid_'+d.pid+'\')">Restore original driver</button>';
        h+='</div></div>';
      }
      h+='</div>';
    }
    if(ordinary.length){
      h+='<h3>Ordinary keyboards — need driver swap</h3>';
      h+='<p class="modal-hint">Before Zadig these are normal keyboards: they type normally and KeySidekick can\'t read them. Identify which VID/PID your keyboard is, then replace its MI_00 driver in Zadig.</p>';
      h+='<div class="device-list">';
      for(let i=0;i<lastDevices.length;i++){
        const d=lastDevices[i];
        if(d.status==='ready')continue;
        h+='<div class="device-card" data-vidpid="'+esc('vid_'+d.vid+'&pid_'+d.pid)+'">';
        h+='<div class="device-info">'+driverBadge(d)+'<span class="device-name">'+esc(d.name||'Unnamed device')+'</span><span class="device-path">'+esc('VID_'+d.vid+' PID_'+d.pid+(d.mi?' [MI_'+d.mi+']':'')+' · '+kindLabels(d))+'</span></div>';
        h+='<div class="device-actions">';
        h+='<button class="btn small" onclick="startIdentify()">Identify by keypress or click</button>';
        h+='<button class="btn small" onclick="wizardToZadig('+i+')">Driver swap guide</button>';
        h+='</div></div>';
      }
      h+='</div>';
    }
  }
  document.getElementById('editor').innerHTML=h;
  selectedProfileName=''; currentView='devices'; sel=null;
}
async function identifyDeviceIdx(idx){
  const d=lastDevices[idx];
  if(!d)return;
  const vp='vid_'+d.vid+'&pid_'+d.pid;
  let devCard=null;
  for(const c of document.querySelectorAll('[data-vidpid]')){
    if(c.getAttribute('data-vidpid')===vp){devCard=c;break;}
  }
  if(!devCard)return;
  devCard.classList.add('identify-active');
  toast('Press a key on this keyboard now…','info');
  try{
    for(let i=0;i<10;i++){
      const r=await api('GET','/api/v1/devices/detect');
      if(r.detected&&r.detected.length>0){
        const found=d.winusbPath&&r.detected.some(x=>x.path===d.winusbPath);
        if(found){
          toast('Keyboard detected!','success');
          devCard.classList.add('identify-found');
          return;
        }
      }
      await new Promise(res=>setTimeout(res,500));
    }
    toast('No keypress detected — check the keyboard is on','error');
  }catch(e){
    toast('Detect failed: '+e.message,'error');
  }finally{ devCard.classList.remove('identify-active'); }
}
async function testDeviceIdx(idx){
  const d=lastDevices[idx];
  if(!d)return;
  toast('Testing device…','info');
  try{
    const r=await api('GET','/api/v1/devices/detect');
    const found=r.detected&&d.winusbPath&&r.detected.some(x=>x.path===d.winusbPath);
    toast(found?'✓ Device responds — keys will be captured!':'Device opened OK, but no key data. Press any key on this keyboard.','info');
  }catch(e){toast('Test failed: '+e.message,'error');}
}
// Per-instance activation: add a specific instance by full path (for two identical keyboards)
async function activateExact(idx){
  const d=lastDevices[idx];
  if(!d||!d.winusbPath){toast('No device path to activate','error');return;}
  try{
    const r=await api('POST','/api/v1/devices/activate',{path:d.winusbPath});
    toast(r.ok?'Activated this exact instance (by path)':('Failed: '+(r.error||'unknown')),r.ok?'success':'error');
  }catch(e){toast('Activate failed: '+e.message,'error');}
}
// Press-a-key identification (Raw Input — also works for ORDINARY keyboards before Zadig)
let identifyLastVp='';
function vkLabel(vk,mk){
  if(!vk)return mk?('0x'+mk.toString(16)):'?';
  if(vk===0x01)return 'Mouse button 1';   // LMB
  if(vk===0x02)return 'Mouse button 2';   // RMB
  if(vk===0x04)return 'Mouse button 3';   // MMB
  if(vk===0x05)return 'Mouse button 4';   // XBUTTON1
  if(vk===0x06)return 'Mouse button 5';   // XBUTTON2
  if(vk>=0x41&&vk<=0x5A)return String.fromCharCode(vk);   // A-Z
  if(vk>=0x30&&vk<=0x39)return String.fromCharCode(vk);   // 0-9
  const m={0x20:'Space',0x0D:'Enter',0x09:'Tab',0x1B:'Esc',0x08:'Backspace',
           0x25:'Left',0x26:'Up',0x27:'Right',0x28:'Down',
           0x2D:'Insert',0x2E:'Delete',0x24:'Home',0x23:'End',0x21:'PageUp',0x22:'PageDown',
           0xBE:'.',0x2F:'.',0xBA:';',0xBF:'/',0xDB:'[',0xDD:']',0x2C:',',0xDC:'\\\\',
           0x60:'.',0x61:'.',0x62:'.',0x63:'.',
           0x70:'F1',0x71:'F2',0x72:'F3',0x73:'F4',0x74:'F5',0x75:'F6',0x76:'F7',0x77:'F8',
           0x78:'F9',0x79:'F10',0x7A:'F11',0x7B:'F12'};
  if(m[vk])return m[vk];
  return mk?('0x'+mk.toString(16)):('vk'+vk);
}
function renderIdentifyFeed(events){
  const feed=document.getElementById('identifyFeed');
  if(!feed)return;
  const items=(events||[]).slice(-12);
  if(!items.length){ feed.innerHTML=''; return; }
  let h='';
  for(let i=0;i<items.length;i++){
    const e=items[i];
    const vpLabel=e.identifiable&&e.vid&&e.pid?('VID_'+e.vid+' PID_'+e.pid+(e.mi?' [MI_'+e.mi+']':'')):null;
    h+='<div class="identify-hit'+(i===items.length-1?' last':'')+'">'
      +'<code class="identify-key">'+esc(vkLabel(e.vk,e.makeCode))+'</code>'
      +' <span class="identify-arrow">→</span> '
      +(vpLabel?'<span class="device-vidpid">'+esc(vpLabel)+'</span>'
               :'<span class="identify-un">can\'t attribute (composite)</span>')
      +'</div>';
  }
  feed.innerHTML=h;
}
async function startIdentify(){
  clearWizardTimers();
  identifyLastVp='';
  const st=document.getElementById('identifyStatus');
  const feed=document.getElementById('identifyFeed');
  if(feed)feed.innerHTML='';
  if(st){st.textContent='';}
  try{
    await api('POST','/api/v1/input/identify',{});
    if(st){st.textContent='Listening — press a key or click a mouse button on the target device…';st.className='prep-status listening';}
    toast('Press a key or click a mouse button on the device you want to identify…','info');
    const startTs=Date.now();
    identifyPollTimer=setInterval(async ()=>{
      let r;
      try{ r=await api('GET','/api/v1/input/identify'); }catch(e){ return; }
      renderIdentifyFeed(r.events||[]);
      // highlight by the last identified press
      const events=r.events||[];
      let lastId=null;
      for(let i=events.length-1;i>=0;i--){ if(events[i].identifiable&&events[i].vid){ lastId=events[i]; break; } }
      if(lastId){
        const vp='vid_'+lastId.vid+'&pid_'+lastId.pid;
        if(vp!==identifyLastVp){ identifyLastVp=vp; highlightDevice(vp); }
        if(st){st.innerHTML='Source: <b>VID_'+esc(lastId.vid)+' PID_'+esc(lastId.pid)+'</b>';st.className='prep-status ok';}
      }else if(r.listening){
        if(st){st.textContent='Windows can\'t attribute this device (composite-device limitation). Pick it from the list by name / VID / PID.';st.className='prep-status warn';}
      }
      if(Date.now()-startTs>15000){
        clearWizardTimers();
        if(!(feed&&feed.innerHTML)){ if(st){st.textContent='No input received. Press a key or click a mouse button on the target device.';st.className='prep-status warn';} }
      }
    },400);
  }catch(e){
    if(st){st.textContent='Identify failed: '+e.message;st.className='prep-status warn';}
    toast('Identify failed: '+e.message,'error');
  }
}
function highlightDevice(vp){
  const cards=document.querySelectorAll('[data-vidpid]');
  for(const c of cards){
    if(c.getAttribute('data-vidpid')===vp){
      c.classList.add('identify-found');
      c.scrollIntoView({behavior:'smooth',block:'center'});
    }
  }
}
function clearWizardTimers(){
  if(wizardPollTimer){clearInterval(wizardPollTimer);wizardPollTimer=null;}
  if(identifyPollTimer){clearInterval(identifyPollTimer);identifyPollTimer=null;}
}
// Останавливает ВСЕ фоновые опросы (wizard Zadig-poll, identify, capture).
// Вызывается при любой смене экрана — иначе опросы живут вечно и дёргают API.
function stopAllPolling(){
  clearWizardTimers();
  if(ct){clearInterval(ct);ct=null;}
  const b=document.getElementById('captureBtn');
  if(b){b.classList.remove('capture-active');b.textContent='Capture from keyboard';}
}
// ---- Wizard state (HID-first onboarding: ordinary → find → prepare → Zadig → verify → mode → done) ----
let wizardState={step:0,selectedUsbId:'',selectedVidPid:'',selectedName:'',verifyPath:''};
async function showWizard(){
  currentView='wizard';
  clearWizardTimers();
  wizardState={step:0,selectedUsbId:'',selectedVidPid:'',selectedName:'',verifyPath:''};
  renderWizard();
}
function wizardProgress(current,total){
  let h='<div class="wizard-progress">';
  for(let i=1;i<=total;i++){
    if(i>1)h+='<span class="wizard-dot'+(i<=current?' done':'')+'"></span>';
    h+='<span class="wizard-step'+(i===current?'-active':'')+(i<current?' wizard-inactive':'')+'">'+i+'</span>';
  }
  return h+'</div>';
}
async function applyDriverSwap(vidpid,label){
  try{
    const r=await api('POST','/api/v1/driver/swap',{vidpid:vidpid});
    if(r.ok){
      toast('Driver swap started for '+vidpid+' — confirm the UAC prompt, then the keyboard reappears automatically','success');
    }else{
      toast('Swap failed: '+(r.error||'unknown'),'error');
    }
  }catch(e){ toast('Swap failed: '+e.message,'error'); }
}
async function restoreOriginalDriver(vidpid){
  try{
    const r=await api('POST','/api/v1/driver/restore',{vidpid:vidpid});
    toast(r.ok?'Restore started — confirm the UAC prompt':('Restore failed: '+(r.error||'unknown')),r.ok?'success':'error');
  }catch(e){ toast('Restore failed: '+e.message,'error'); }
}
function vidPidParts(vp){
  const m=String(vp||'').match(/vid_([0-9a-f]{4})&pid_([0-9a-f]{4})/i);
  return m?{vid:m[1].toUpperCase(),pid:m[2].toUpperCase()}:{vid:'',pid:''};
}
function matchDevice(d){
  if(wizardState.selectedUsbId&&d.usbId===wizardState.selectedUsbId)return true;
  if(wizardState.selectedVidPid&&d.vid&&d.pid&&('vid_'+d.vid+'&pid_'+d.pid)===wizardState.selectedVidPid)return true;
  return false;
}
function wizardDeviceCard(d,idx){
  const vp=d.vid&&d.pid?('vid_'+d.vid+'&pid_'+d.pid):'';
  return '<div class="device-card wizard-device" data-vidpid="'+esc(vp)+'" onclick="wizardSelectDeviceIdx('+idx+')">'
    +'<div class="device-info">'+driverBadge(d)
    +'<span class="device-name">'+esc(d.name||'Unnamed device')+'</span>'
    +'<span class="device-path">'+esc(vp?(vp.toUpperCase()+(d.mi?' [MI_'+d.mi+']':'')):(d.usbId||''))+' · '+kindLabels(d)+'</span>'
    +'</div>'
    +'<div class="device-check"><span class="device-check-icon">→</span></div></div>';
}
function wizardSelectDeviceIdx(idx){
  const d=lastDevices[idx];
  if(!d)return;
  wizardState.selectedUsbId=d.usbId||'';
  wizardState.selectedVidPid=(d.vid&&d.pid)?('vid_'+d.vid+'&pid_'+d.pid):'';
  wizardState.selectedName=d.name||'';
  clearWizardTimers();
  wizardState.step=(d.status==='ready')?4:2;   // ready → verify+activate; ordinary → prepare
  renderWizard();
}
function wizardBack(){
  clearWizardTimers();
  if(wizardState.step>0)wizardState.step--;
  renderWizard();
}
function wizardToZadig(idx){
  const d=lastDevices[idx];
  if(!d)return;
  wizardState.selectedUsbId=d.usbId||'';
  wizardState.selectedVidPid=(d.vid&&d.pid)?('vid_'+d.vid+'&pid_'+d.pid):'';
  wizardState.selectedName=d.name||'';
  if(!prepDone()){
    showPrepGateDialog();
    return;
  }
  currentView='wizard';
  wizardState.step=3;
  renderWizard();
}
// ---- Prep-checklist gate (persisted so the Devices page can enforce it) ----
function prepStateSave(typed,auto,fallback){
  try{localStorage.setItem('ks_prep_state',JSON.stringify({typed:!!typed,auto:!!auto,fallback:!!fallback}));}catch(e){}
}
function prepStateLoad(){
  try{const s=JSON.parse(localStorage.getItem('ks_prep_state')||'null');return (s&&typeof s==='object')?s:{};}catch(e){return {};}
}
function prepDone(){
  const s=prepStateLoad();
  return !!(s.typed&&s.auto&&s.fallback);
}
function prepMissingItems(){
  const s=prepStateLoad(),out=[];
  if(!s.typed)out.push('typing test not done (the keyboard was not verified to type normally)');
  if(!s.auto)out.push('auto-start with Windows not enabled');
  if(!s.fallback)out.push('fallback input not confirmed (no second keyboard / on-screen keyboard)');
  return out;
}
function showPrepGateDialog(){
  const missing=prepMissingItems();
  const items=missing.length?missing.map(function(x){return '<li>'+esc(x)+'</li>';}).join(''):'';
  let h='<div class="modal-overlay" onclick="if(event.target===this)closeModal()"><div class="modal" role="dialog" aria-modal="true" aria-label="Before the driver swap"><div class="modal-header"><h3>Before the driver swap</h3><button class="btn small" aria-label="Close" onclick="closeModal()">✕</button></div><div class="modal-body">'
  +'<p class="modal-hint">After the driver swap this keyboard stops typing on its own — all keys go through KeySidekick. These preparation steps are still unfinished:</p>'
  +'<ul style="margin:0 0 8px;padding-left:18px;color:var(--muted);font-size:12px;line-height:1.7">'+items+'</ul>'
  +'<p class="modal-hint" style="margin-top:8px"><b>Risk:</b> with auto-start off, the keyboard types only while sidekick runs — after a reboot you may have no keyboard input at all.</p>'
  +'<div class="ab-actions"><button class="btn" onclick="wizardPrepFirst()">Go to prep first</button><button class="btn primary" onclick="wizardProceedAnyway()">Proceed anyway</button></div>'
  +'</div></div></div>';
  const m=document.getElementById('appModal');
  if(!m)return;
  m.innerHTML=h;m.style.display='block';
  _modalPreviousFocus=document.activeElement;
  document.addEventListener('keydown',_modalKeyHandler);
}
function wizardPrepFirst(){
  closeModal();
  currentView='wizard';
  wizardState.step=2;
  renderWizard();
}
function wizardProceedAnyway(){
  closeModal();
  currentView='wizard';
  wizardState.step=3;
  renderWizard();
}
async function renderWizard(){
  clearWizardTimers();
  if(wizardState.step===0){
    // Step 0: "your keyboard is ordinary" — honest starting point before Zadig
    let h='<div class="wizard-page">';
    h+=wizardProgress(1,7);
    h+='<h2>Set up your keyboard</h2>';
    h+='<div class="info-callout warn">';
    h+='<strong>⚠️ Right now your keyboard is an ordinary keyboard</strong>';
    h+='<p>Until its driver is replaced, KeySidekick cannot read it or tell it apart from other keyboards — Windows merges all keyboards into one input stream (for composite devices Raw Input gives no per-device identity).</p>';
    h+='<p style="margin-top:8px;color:var(--muted);font-size:13px">To make KeySidekick capture it, the keyboard\'s <code>MI_00</code> interface driver is replaced from <code>hidusb.sys</code> to <code>WinUSB.sys</code> with the free tool <strong>Zadig</strong> (zadig.akeo.ie).</p>';
    h+='<p style="margin-top:8px;color:var(--warn);font-size:13px"><strong>Important:</strong> after the swap the keyboard stops typing on its own — all keys go through KeySidekick. That\'s why preparation (auto-start, fallback input) comes first.</p>';
    h+='</div>';
    h+='<p><strong>Ready to find and convert your keyboard?</strong></p>';
    h+='<div style="margin-top:16px"><button class="btn primary" onclick="wizardState.step=1;renderWizard()">Find my keyboard</button></div>';
    h+='</div>';
    document.getElementById('editor').innerHTML=h;
  }else if(wizardState.step===1){
    // Step 1: all input devices + press-based identification (Raw Input)
    const r=await api('GET','/api/v1/hid');
    lastDevices=r.devices||[];
    const keyboards=lastDevices.filter(d=>d.status==='ready'||(d.kinds||'').split(',').indexOf('keyboard')>=0);
    const others=lastDevices.filter(d=>!(d.status==='ready'||(d.kinds||'').split(',').indexOf('keyboard')>=0));
    let h='<div class="wizard-page">';
    h+=wizardProgress(2,7);
    h+='<h2>Find your keyboard</h2>';
    h+='<p class="modal-hint">All keyboards, mice and smart devices with keyboards are listed. Before Zadig your keyboard is an ordinary one — identify it by name / VID / PID, or press a key / click a mouse button on it.</p>';
    h+='<div style="display:flex;gap:8px;align-items:center;margin:12px 0">';
    h+='<button class="btn" onclick="startIdentify()">⌨ Press a key or click a mouse button to identify</button>';
    h+='<button class="btn quiet" onclick="renderWizard()">Refresh list</button>';
    h+='</div>';
    h+='<div id="identifyStatus" class="prep-status"></div>';
    h+='<div id="identifyFeed" class="identify-feed"></div>';
    if(!lastDevices.length){
      h+='<div class="info-callout"><strong>No devices found.</strong><span>Connect your keyboard/mouse and click "Refresh list".</span></div>';
    }else{
      if(keyboards.length){
        h+='<h3>Keyboards</h3><div class="device-list">';
        for(let i=0;i<lastDevices.length;i++){ if(keyboards.indexOf(lastDevices[i])>=0)h+=wizardDeviceCard(lastDevices[i],i); }
        h+='</div>';
      }
      if(others.length){
        h+='<h3>Other input devices (mice, media, …)</h3><div class="device-list">';
        for(let i=0;i<lastDevices.length;i++){ if(others.indexOf(lastDevices[i])>=0)h+=wizardDeviceCard(lastDevices[i],i); }
        h+='</div>';
      }
    }
    h+='<div class="wizard-actions"><button class="btn" onclick="wizardBack()">Back</button></div>';
    h+='</div>';
    document.getElementById('editor').innerHTML=h;
  }else if(wizardState.step===2){
    // Step 2: preparation BEFORE the driver swap (careful work in a safe state)
    let h='<div class="wizard-page">';
    h+=wizardProgress(3,7);
    h+='<h2>Prepare before the driver swap</h2>';
    h+='<p class="modal-hint">Device <b>'+esc(wizardState.selectedVidPid||'—')+'</b> ('+esc(wizardState.selectedName||'')+') is currently a normal keyboard. After the swap it types only through KeySidekick. Complete these first:</p>';
    h+='<div class="prep-list">';
    h+='<div class="prep-item"><strong>1. It types as a normal keyboard</strong><p>Type a few characters here — they should appear:</p><input id="prepTypeTest" placeholder="Type here…" style="width:100%" oninput="updatePrepReady()"><p class="prep-hint">If characters appear, the keyboard works normally — this is the safe state, nothing is changed yet.</p></div>';
    h+='<div class="prep-item"><strong>2. KeySidekick starts with Windows</strong><p>After the swap the keyboard only types while sidekick.exe runs — enable auto-start first:</p><button class="btn" id="prepAutostartBtn" onclick="enableAutostart()">Enable auto-start with Windows</button> <span id="prepAutostartStatus" class="prep-status"></span></div>';
    h+='<div class="prep-item"><strong>3. Fallback input ready</strong><label class="prep-check"><input type="checkbox" id="prepFallback" onchange="updatePrepReady()"> I have another keyboard or the on-screen keyboard (osk) available</label></div>';
    h+='<div class="prep-item"><strong>4. Which interface to swap</strong><p>Swap <b>MI_00</b> (keyboard) only. Do <b>NOT</b> touch <b>MI_01</b> (mouse/touchpad) — wrong interface breaks the mouse.</p></div>';
    h+='<div class="prep-item"><strong>5. Zadig ready</strong><p>Free and open-source: zadig.akeo.ie. Run it <b>as administrator</b>.</p></div>';
    h+='</div>';
    h+='<div class="wizard-actions"><button class="btn" onclick="wizardBack()">Back</button><button class="btn primary" id="wizardPrepNext" onclick="wizardState.step=3;renderWizard()" disabled>Show driver swap steps</button></div>';
    h+='</div>';
    document.getElementById('editor').innerHTML=h;
    try{
      const s=await api('GET','/api/v1/startup');
      const st=document.getElementById('prepAutostartStatus');
      if(st&&s.installed){ st.textContent='✓ Already enabled'; st.dataset.ok='1'; st.className='prep-status ok'; }
    }catch(e){}
  }else if(wizardState.step===3){
    // Step 3: driver swap in Zadig (for the chosen VID/PID, MI_00)
    const p=vidPidParts(wizardState.selectedVidPid);
    let h='<div class="wizard-page">';
    h+=wizardProgress(4,7);
    h+='<h2>Replace the driver (Zadig)</h2>';
    h+='<p class="modal-hint">For <b>'+esc(wizardState.selectedVidPid||'your keyboard')+'</b>. The wizard watches for the device to flip to WinUSB.</p>';
    h+='<div class="prep-item"><strong>Automatic swap (recommended)</strong>'
      +'<p>KeySidekick can bind the Microsoft-signed WinUSB driver itself — no Zadig needed (Windows 10 1809+).</p>'
      +'<button class="btn primary" id="wizardAutoSwapBtn" onclick="applyDriverSwap(\''+esc(wizardState.selectedVidPid||'')+'\',\'wizard\')">Swap automatically (UAC prompt)</button>'
      +' <span class="prep-hint">After the swap the list above flips to WinUSB-ready — press Continue.</span></div>';
    h+='<details class="advanced" style="margin:12px 0"><summary>Or do it manually with Zadig (fallback)</summary>';
    h+='<ol class="zadig-steps">';
    h+='<li>Open <b>Zadig</b> as administrator.</li>';
    h+='<li>Menu <b>Options → List All Devices</b> (enable it).</li>';
    h+='<li>In the dropdown find <code>USB Input Device (VID '+(p.vid||'xxxx')+' PID '+(p.pid||'yyyy')+') [MI 00]</code> — <b>MI 00</b>, not MI 01 (that\'s the mouse).</li>';
    h+='<li>Set the target driver to <b>WinUSB</b> (use the ▲▼ arrows).</li>';
    h+='<li>Click <b>Replace Driver</b> (or Install Driver) and confirm.</li>';
    h+='<li>Wait for completion (~10-30 seconds).</li>';
    h+='</ol></details>';
    h+='<div class="info-callout warn"><strong>Still typing like normal after Zadig says SUCCESS?</strong><span>Zadig may have replaced a phantom copy. Select the device in Zadig and click <b>Reinstall Driver</b> (not Replace).</span></div>';
    h+='<div id="zadigStatus" class="prep-status">Watching for the device to become WinUSB-ready…</div>';
    h+='<div class="wizard-actions"><button class="btn" onclick="wizardBack()">Back</button><button class="btn primary" id="wizardZadigNext" onclick="wizardState.step=4;renderWizard()" disabled>Continue</button></div>';
    h+='</div>';
    document.getElementById('editor').innerHTML=h;
    startZadigPoll();
  }else if(wizardState.step===4){
    // Step 4: verification (service==WinUSB) + keypress detection + activation
    let dev=null;
    try{ const r=await api('GET','/api/v1/hid'); dev=(r.devices||[]).find(matchDevice); }catch(e){}
    wizardState.verifyPath=(dev&&dev.winusbPath)||'';
    let h='<div class="wizard-page">';
    h+=wizardProgress(5,7);
    h+='<h2>Verify it works</h2>';
    if(dev&&dev.status==='ready'&&dev.winusbPath){
      h+='<p class="modal-hint">Device <b>'+esc(wizardState.selectedVidPid||'')+'</b> is now on the WinUSB driver (phantom-check passed: service is '+esc(dev.service||'WinUSB')+').</p>';
      h+='<div class="prep-item"><strong>Press a key to test</strong><p>KeySidekick should capture keys from this keyboard.</p><button class="btn" onclick="wizardVerifyPress()">Press a key to test</button> <span id="wizardVerifyResult" class="prep-status"></span></div>';
      h+='<div class="prep-item"><strong>Make it the active keyboard</strong><p>Add <b>'+esc(wizardState.selectedVidPid||'')+'</b> to the config (DeviceVIDPID) and reconnect the hot path.</p><button class="btn" id="wizardActivateBtn" onclick="wizardActivate()">Make active</button> <span id="wizardActivateResult" class="prep-status"></span></div>';
      h+='<div class="wizard-actions"><button class="btn" onclick="wizardBack()">Back</button><button class="btn primary" id="wizardVerifyNext" onclick="wizardState.step=5;renderWizard()" disabled>Next</button></div>';
    }else{
      h+='<div class="info-callout warn"><strong>Device not ready yet.</strong><span>It\'s not on the WinUSB driver yet. Go back to the driver-swap step and complete the Zadig replacement.</span></div>';
      h+='<div class="wizard-actions"><button class="btn" onclick="wizardBack()">Back</button><button class="btn primary" onclick="wizardState.step=3;renderWizard()">Go to driver swap</button></div>';
    }
    h+='</div>';
    document.getElementById('editor').innerHTML=h;
  }else if(wizardState.step===5){
    // Step 5: profile mode
    let h='<div class="wizard-page">';
    h+=wizardProgress(6,7);
    h+='<h2>What should this keyboard do?</h2>';
    h+='<p class="modal-hint">After setup, you can always change this in the dashboard.</p>';
    h+='<div class="wizard-options">';
    h+='<div class="wizard-option" onclick="wizardMode(\'basic\')"><div class="wizard-option-icon">⌨️</div><h3>Type normally</h3><p>All keys type like a regular keyboard. Useful for macros with text.</p></div>';
    h+='<div class="wizard-option" onclick="wizardMode(\'targeted\')"><div class="wizard-option-icon">🎯</div><h3>Control an app</h3><p>Keys are routed to one application in background (e.g. AIMP).</p></div>';
    h+='<div class="wizard-option" onclick="wizardMode(\'custom\')"><div class="wizard-option-icon">⚙️</div><h3>Custom mix</h3><p>Some keys type, some control apps. Best for power users.</p></div>';
    h+='<div class="wizard-option" onclick="showPresetWizard()"><div class="wizard-option-icon">📦</div><h3>Ready-made pad</h3><p>Start from a template — AI agent, media, OBS, meetings, PowerPoint.</p></div>';
    h+='</div>';
    h+='</div>';
    document.getElementById('editor').innerHTML=h;
  }else if(wizardState.step===6){
    // Step 6: done — the keyboard is no longer ordinary
    let h='<div class="wizard-page">';
    h+=wizardProgress(7,7);
    h+='<div class="wizard-done">';
    h+='<div class="wizard-check">✓</div>';
    h+='<h2>Ready to go</h2>';
    let modeText='';
    if(wizardState.mode==='basic')modeText='It types normally in the focused app — the <b>Basic</b> profile is active.';
    else if(wizardState.mode==='targeted')modeText='Keys are routed to <b>'+esc(wizardState.createdAppName||'your app')+'</b> — open the dashboard to add mappings.';
    else if(wizardState.mode==='custom')modeText='The <b>Custom pad</b> profile is active — open the dashboard to add mappings (mix of typing and app control).';
    else modeText='Keys are routed by your profile — open the dashboard to add mappings.';
    h+='<p>Your keyboard is set up. It is <b>no longer an ordinary keyboard</b>: it types only while KeySidekick is running — '+modeText+'</p>';
    h+='<div id="wizardDoneAutoStatus" class="prep-status">Checking auto-start…</div>';
    h+='<p style="font-size:13px;color:var(--muted);margin-top:8px">To make it a normal keyboard again: Device Manager → uninstall the WinUSB device → Action → Scan for hardware changes.</p>';
    h+='<div class="wizard-actions">';
    h+='<button class="btn primary" onclick="currentView=\'profile\';refresh();">Open dashboard</button>';
    h+='<button class="btn" onclick="showDevices()">Back to devices</button>';
    h+='</div>';
    h+='</div>';
    h+='</div>';
    document.getElementById('editor').innerHTML=h;
    // Честная проверка auto-start: если НЕ включён — предупредить, а не утверждать обратное.
    (async()=>{
      const st=document.getElementById('wizardDoneAutoStatus');
      try{
        const s=await api('GET','/api/v1/startup');
        if(st){
          if(s.installed){
            st.textContent='✓ Auto-start with Windows is enabled — the keyboard will work after reboot.';
            st.className='prep-status ok';
          }else{
            st.innerHTML='<b>⚠ Auto-start is OFF.</b> After a reboot the keyboard won\'t type until you start KeySidekick manually. <button class="btn small" onclick="enableAutostart().then(function(){var x=document.getElementById(\'wizardDoneAutoStatus\');if(x){x.textContent=\'✓ Auto-start enabled\';x.className=\'prep-status ok\';}})">Enable now</button>';
            st.className='prep-status warn';
          }
        }
      }catch(e){ if(st){st.textContent='Could not check auto-start status.';st.className='prep-status warn';} }
    })();
  }else if(wizardState.step===1.5){
    // Step 1.5: choosing an application for targeted mode
    let h='<div class="wizard-page">';
    h+=wizardProgress(6,7);
    h+='<h2>Which application should it control?</h2>';
    h+='<p class="modal-hint">Pick a running app or browse for an executable.</p>';
    h+='<div id="wizardAppPicker">';
    h+='<div class="field"><label for="wizardAppPath">Application path (e.g. C:\\Program Files\\AIMP\\AIMP.exe)</label><input id="wizardAppPath" placeholder="C:\\...\\app.exe" style="width:100%" oninput="document.getElementById(\'wizardConfirm\').disabled=!this.value.trim()"></div>';
    h+='<div style="display:flex;gap:8px;margin-top:8px">';
    h+='<button class="btn" onclick="wizardPickFromRunning()">Pick from running windows</button>';
    h+='<button class="btn primary" id="wizardConfirm" onclick="wizardConfirmApp()" disabled>Continue</button>';
    h+='</div>';
    h+='</div>';
    h+='</div>';
    document.getElementById('editor').innerHTML=h;
  }
}
function startZadigPoll(){
  clearWizardTimers();
  wizardPollTimer=setInterval(async ()=>{
    try{
      const r=await api('GET','/api/v1/hid');
      const dev=(r.devices||[]).find(matchDevice);
      if(dev&&dev.status==='ready'){
        clearWizardTimers();
        const st=document.getElementById('zadigStatus');
        if(st){st.textContent='✓ Device is ready (WinUSB) — press Continue.';st.className='prep-status ok';}
        const next=document.getElementById('wizardZadigNext');
        if(next)next.disabled=false;
        toast('Keyboard is now WinUSB-ready!','success');
      }
    }catch(e){}
  },3000);
}
async function wizardVerifyPress(){
  const el=document.getElementById('wizardVerifyResult');
  const path=wizardState.verifyPath;
  if(el){el.textContent='Press a key on this keyboard now…';el.className='prep-status listening';}
  try{
    for(let i=0;i<12;i++){
      const r=await api('GET','/api/v1/devices/detect');
      const found=(r.detected&&r.detected.length>0)&&(!path||r.detected.some(d=>d.path===path));
      if(found){
        if(el){el.textContent='✓ Key captured!';el.className='prep-status ok';}
        const next=document.getElementById('wizardVerifyNext');
        if(next)next.disabled=false;
        toast('Keyboard detected!','success');
        return;
      }
      await new Promise(res=>setTimeout(res,500));
    }
    if(el){el.textContent='No key captured. Make sure the keyboard is on and press its keys.';el.className='prep-status warn';}
  }catch(e){ if(el){el.textContent='Test failed: '+e.message;el.className='prep-status warn';} }
}
async function wizardActivate(){
  const btn=document.getElementById('wizardActivateBtn');
  const el=document.getElementById('wizardActivateResult');
  if(btn)btn.disabled=true;
  try{
    const r=await api('POST','/api/v1/devices/activate',{vidpid:wizardState.selectedVidPid});
    if(r.ok){
      if(el){el.textContent='✓ Activated: '+wizardState.selectedVidPid;el.className='prep-status ok';}
      const next=document.getElementById('wizardVerifyNext');
      if(next)next.disabled=false;
      toast('Keyboard is now the active device','success');
      // Portable-сценарий: профили из config.ini уже под рукой — показываем сколько.
      try{
        const pr=await api('GET','/api/profiles');
        const n=(pr.profiles||[]).length;
        if(n>0){
          toast('Profiles ready ('+n+') — your portable setup is fully restored','success');
        }
      }catch(e){}
    }else{
      if(el){el.textContent='Failed: '+(r.error||'unknown');el.className='prep-status warn';}
      toast('Activate failed: '+(r.error||'unknown'),'error');
    }
  }catch(e){
    if(el){el.textContent='Failed: '+e.message;el.className='prep-status warn';}
  }finally{ if(btn)btn.disabled=false; }
}
async function enableAutostart(){
  const btn=document.getElementById('prepAutostartBtn');
  const st=document.getElementById('prepAutostartStatus');
  if(btn)btn.disabled=true;
  try{
    const r=await api('POST','/api/v1/startup',{enabled:"true"});
    const ok=(r.ok===true&&r.installed===true);
    const shortFail=(r.ok===true&&r.installed===false);
    if(st){st.textContent=ok?'✓ Auto-start enabled':(shortFail?'Shortcut creation failed':'Failed: '+(r.error||'unknown'));st.dataset.ok=ok?'1':'0';st.className=ok?'prep-status ok':'prep-status warn';}
    toast(ok?'Auto-start enabled':(shortFail?'Shortcut creation failed':'Auto-start failed'),ok?'success':'error');
  }catch(e){
    if(st){st.textContent='Failed: '+e.message;st.dataset.ok='0';st.className='prep-status warn';}
  }finally{
    if(btn)btn.disabled=false;
    updatePrepReady();
  }
}
function updatePrepReady(){
  const fallback=document.getElementById('prepFallback');
  const auto=document.getElementById('prepAutostartStatus');
  const typed=document.getElementById('prepTypeTest');
  const typedOk=!!(typed&&typed.value.length>0);
  const autoOk=!!(auto&&auto.dataset.ok==='1');
  const fallbackOk=!!(fallback&&fallback.checked);
  const next=document.getElementById('wizardPrepNext');
  const ok=typedOk&&autoOk&&fallbackOk;
  if(next)next.disabled=!ok;
  prepStateSave(typedOk,autoOk,fallbackOk);
}
async function wizardMode(mode){
  wizardState.mode=mode;
  if(mode==='basic'){
    // Создать (или переиспользовать) basic-профиль и активировать его.
    try{ await api('POST','/api/v1/profile/create',{id:'wizard-basic',name:'Basic',mode:'basic'}); }catch(e){/* уже есть */}
    try{
      const act=await api('POST','/api/profile/activate',{name:'Basic'});
      toast(act.ok?'Profile "Basic" is active':'Basic profile is ready','success');
    }catch(e){ toast('Failed to activate Basic','error'); }
    wizardState.step=6;
    renderWizard();
  }else if(mode==='targeted'){
    // Сначала выбираем приложение — дальше wizardConfirmApp реально сохраняет
    // профиль + цель приложения + активирует.
    wizardState.step=1.5; // intermediate: pick target app
    renderWizard();
  }else{
    // custom mix — пустой targeted-профиль, маппинги пользователь добавит сам.
    try{
      const profName='Custom pad';
      try{ await api('POST','/api/v1/profile/create',{id:'wizard-custom',name:profName,mode:'targeted'}); }catch(e){}
      const act=await api('POST','/api/profile/activate',{name:profName});
      wizardState.createdProfileName=profName;
      toast(act.ok?'Profile "Custom pad" is active':'Custom pad is ready','success');
    }catch(e){ toast('Failed to create Custom pad','error'); }
    wizardState.step=6;
    renderWizard();
  }
}
async function wizardPickFromRunning(){
  try{
    const r=await api('GET','/api/v1/windows/foreground');
    if(!r.found){
      toast('No foreground window detected','error');
      return;
    }
    const input=document.getElementById('wizardAppPath');
    if(input){
      input.value=(r.processPath||'')+(r.windowClass?' ('+r.windowClass+')':'');
      // enable continue
      document.getElementById('wizardConfirm').disabled=false;
      toast('Selected: '+(r.processName||r.title||r.windowClass),'success');
    }
  }catch(e){
    toast('Cannot detect foreground window: '+e.message,'error');
  }
}
async function wizardConfirmApp(){
  const pathInput=document.getElementById('wizardAppPath');
  const path=pathInput?pathInput.value.trim():'';
  if(!path){
    toast('Enter application path first','error');
    return;
  }
  const btn=document.getElementById('wizardConfirm');
  if(btn)btn.disabled=true;
  try{
    // Формат поля: "C:\path\app.exe" или "C:\path\app.exe (WindowClass)"
    const m=path.match(/^(.*?)\s*\(([^()]*)\)\s*$/);
    const exePath=m?m[1].trim():path;
    const windowClass=m?m[2].trim():'';
    const exeName=exePath.split(/[\\\/]/).pop()||'app';
    const appR=await api('POST','/api/v1/applications/create',
      {name:exeName,windowClass:windowClass,exePath:exePath,processName:exeName});
    if(!appR.ok||!appR.id){toast('Failed to save app: '+(appR.error||'unknown'),'error');return;}
    const profName=exeName+' pad';
    const profId='wizard-target-'+Date.now();
    const pr=await api('POST','/api/v1/profile/create',{id:profId,name:profName,mode:'targeted'});
    if(!pr.ok){toast('Failed to create profile: '+(pr.error||'unknown'),'error');return;}
    await api('POST','/api/v1/profile/link-app',{profileId:profId,appId:appR.id});
    await api('POST','/api/v1/profile/set-default-app',{profileId:profId,appId:appR.id});
    const act=await api('POST','/api/profile/activate',{name:profName});
    wizardState.createdProfileName=profName;
    wizardState.createdAppName=exeName;
    toast(act.ok?'Profile "'+profName+'" is active':('Profile created: '+profName),act.ok?'success':'info');
    wizardState.step=6;
    renderWizard();
  }catch(e){
    toast('Failed: '+e.message,'error');
  }finally{
    if(btn)btn.disabled=false;
  }
}
// ---- Phase 7: Diagnostics + Help ----
async function showDiagnostics(){
  stopAllPolling();
  currentView='diagnostics';
  renderSidebar();
  const r=await api('GET','/api/v1/diagnostics');
  let h='<div class="toolbar"><h2>Diagnostics</h2></div>';
  h+='<div class="diag-section"><h3>Device</h3>';
  h+='<table class="diag-table">';
  h+='<tr><td>Status</td><td class="'+(r.device==='connected'?'diag-ok':'diag-err')+'">'+r.device+'</td></tr>';
  h+='<tr><td>WinUSB Handle</td><td>'+(r.winusbHandle?'<span class="diag-ok">open</span>':'<span class="diag-err">closed</span>')+'</td></tr>';
  h+='<tr><td>Pipe ID</td><td><code>'+r.pipeId+'</code></td></tr>';
  h+='<tr><td>VID/PID</td><td><code>'+r.vidpid+'</code></td></tr>';
  h+='<tr><td>Enumerated</td><td>'+(r.deviceEnumerated?'<span class="diag-ok">yes</span>':'<span class="diag-err">no</span>')+'</td></tr>';
  if(r.devicePath)h+='<tr><td>Path</td><td><code class="diag-path">'+esc(r.devicePath)+'</code></td></tr>';
  h+='</table></div>';
  h+='<div class="diag-section"><h3>Config</h3>';
  h+='<table class="diag-table">';
  h+='<tr><td>File</td><td><code>'+r.configFile+'</code></td></tr>';
  h+='<tr><td>Exists</td><td>'+(r.configExists?'<span class="diag-ok">yes</span>':'<span class="diag-err">no</span>')+'</td></tr>';
  h+='<tr><td>Profiles</td><td>'+r.profileCount+'</td></tr>';
  h+='<tr><td>Applications</td><td>'+r.appCount+'</td></tr>';
  h+='<tr><td>Active profile</td><td>'+esc(r.activeProfile)+'</td></tr>';
  h+='</table></div>';
  h+='<div class="diag-section"><h3>API</h3>';
  h+='<table class="diag-table">';
  h+='<tr><td>HTTP Port</td><td>'+r.httpPort+'</td></tr>';
  h+='<tr><td>HTTP Enabled</td><td>'+(r.httpEnabled?'yes':'no')+'</td></tr>';
  h+='<tr><td>Tray Enabled</td><td>'+(r.trayEnabled?'yes':'no')+'</td></tr>';
  h+='</table></div>';
  if(r.driverInfo){
    h+='<div class="diag-section"><h3>Driver</h3>';
    h+='<p class="diag-note">'+esc(r.driverInfo.note)+'</p>';
    h+='<p>WinUSB GUID: <code>'+esc(r.driverInfo.winusbGuid)+'</code></p>';
    h+='</div>';
  }
  if(r.recentLog&&r.recentLog.length>0){
    h+='<div class="diag-section"><h3>Recent log ('+r.recentLog.length+' lines)</h3>';
    h+='<pre class="diag-log">';
    for(const l of r.recentLog)h+=esc(l)+'\n';
    h+='</pre></div>';
  }
  document.getElementById('editor').innerHTML=h;
}
function showHelp(){
  currentView='help';
  renderSidebar();
  let h='<div class="toolbar"><h2>Help / Setup</h2></div>';
  h+='<div class="help-section warn"><h3>⚠️ Driver Replacement Notice</h3>';
  h+='<p><strong>KeySidekick requires replacing your keyboard\'s MI_00 driver with WinUSB.</strong> This is done manually via Zadig (zadig.akeo.ie) and is <em>not</em> reversible by simply uninstalling KeySidekick.</p>';
  h+='<p style="color:var(--warn);margin-top:8px"><strong>Important:</strong> With WinUSB driver, your keyboard will not type unless sidekick.exe is running. This is harmless but expect it to behave differently than normal.</p>';
  h+='</div>';
  h+='<div class="help-section"><h3>How KeySidekick works</h3>';
  h+='<p>KeySidekick turns a dedicated USB keyboard into a control panel for Windows apps. The keyboard\'s MI_00 interface driver was replaced with WinUSB (via Zadig), so its keys no longer reach the OS as normal keystrokes. Instead, <code>sidekick.exe</code> reads raw HID reports and routes them to profiles.</p>';
  h+='<p><strong>If sidekick is not running, the dedicated keyboard will not type anything.</strong> This is expected.</p>';
  h+='</div>';
  h+='<div class="help-section"><h3>Profiles</h3>';
  h+='<ul>';
  h+='<li><strong>basic (typing)</strong> — keys are re-injected via SendInput, so the keyboard types normally. Action keys (like <code>!switch:aimp</code>) switch profiles.</li>';
  h+='<li><strong>targeted (app control)</strong> — keys are routed to a specific app window (e.g. AIMP) without stealing focus. Single keys support normal hold and repeat.</li>';
  h+='<li>One profile can link multiple applications. Use the multi-app action builder (Phase 5+) to route keys to specific apps.</li>';
  h+='</ul></div>';
  h+='<div class="help-section"><h3>Actions</h3>';
  h+='<ul>';
  h+='<li><code>{F1}</code>, <code>{Media_Play_Pause}</code> — send key/media to target</li>';
  h+='<li><code>!switch:aimp</code> — switch to profile "aimp"</li>';
  h+='<li><code>!toggle:basic</code> — toggle between current and "basic"</li>';
  h+='<li><code>!launch:C:\\path\\app.exe</code> — launch application</li>';
  h+='<li><code>!app:Spotify:{Media_Next_Track}</code> — send to specific app</li>';
  h+='</ul></div>';
  h+='<div class="help-section warn"><h3>⚠️ Anti-cheat & Gaming Risks</h3>';
  h+='<p><strong>Online games with EAC (EasyAntiCheat), Vanguard, BattlEye may flag injected input from SendInput/PostMessage.</strong> KeySidekick sends simulated keystrokes, not polled input.</p>';
  h+='<ul>';
  h+='<li><strong>Basic mode</strong> — re-injects via SendInput; may be detected by anti-cheat. Avoid in competitive online games.</li>';
  h+='<li><strong>Targeted mode</strong> — sends to specific app window; safer but still detectable.</li>';
  h+='<li><strong>Auto-switch</strong> — switches profile on foreground window change (currently enabled by default).</li>';
  h+='<li>Recommendation: use targeted mode for productivity tools, disable KeySidekick entirely for competitive games.</li>';
  h+='</ul></div>';
  h+='<div class="help-section"><h3>Wireless Keyboard Notes</h3>';
  h+='<p>Many wireless keyboards (2.4G dongle) go to sleep to save battery. After resume, WinUSB pipe may not deliver key reports until replugged. This is a hardware/driver limitation — sidekick auto-reconnects on resume.</p>';
  h+='<ul>';
  h+='<li>Check battery level before setup (wireless keyboards often stop sending without charge)</li>';
  h+='<li>Re-pair with dongle if keys stop responding (hold pairing button on both)</li>';
  h+='<li>Prefer wired connection for stability</li>';
  h+='</ul></div>';
  h+='<div class="help-section"><h3>Driver Rollback (restore HID)</h3>';
  h+='<p>To make the keyboard work as a normal system keyboard again (without sidekick):</p>';
  h+='<ol>';
  h+='<li>Open <strong>Device Manager</strong></li>';
  h+='<li>Find the keyboard under "Universal Serial Bus devices" (look for WinUSB or the VID/PID)</li>';
  h+='<li>Right-click → Update driver → Browse my computer → Let me pick</li>';
  h+='<li>Select "USB Input Device" (the original HID driver)</li>';
  h+='<li>Or use Zadig again: Options → List All Devices → select the keyboard → replace WinUSB with WinUSB/HID</li>';
  h+='</ol></div>';
  document.getElementById('editor').innerHTML=h;
}
// ---- AI-agent preset wizard ("+ Agent pad") ----
let presetState={step:0,agent:null,target:null};let windowPresetList=[];
async function showPresetWizard(){
  stopAllPolling();
  stopAllPolling();
  currentView='preset';
  clearWizardTimers();
  presetState={step:0,agent:null,target:null};
  windowPresetList=[];
  renderPresetWizard();
}
async function renderPresetWizard(){
  if(presetState.step===0){
    let presets=[];
    try{ const r=await api('GET','/api/v1/presets'); presets=r.presets||[]; }catch(e){}
    windowPresetList=presets;
    let h='<div class="wizard-page">';
    h+=wizardProgress(1,2);
    h+='<h2>Pad templates</h2>';
    h+='<p class="modal-hint">Turn a spare keyboard into a ready-made control pad: pick a scenario and get a profile with F1–F8 mapped. Like a Stream Deck / Codex Micro — on the hardware you already have.</p>';
    if(!presets.length){
      h+='<div class="info-callout"><strong>No presets available.</strong><span>Make sure sidekick exposes /api/v1/presets.</span></div>';
    }else{
      h+='<div class="device-list">';
      for(let i=0;i<presets.length;i++){
        const pst=presets[i];
        const isAgent=pst.kind==='agent';
        const legend=(pst.keys||[]).slice(0,8).map(k=>keyName(k.usage)+' → '+String(k.label||k.action).split('(')[0].trim()).join(' · ');
        h+='<div class="device-card wizard-device" onclick="presetAgent('+i+')">';
        h+='<div class="device-info"><span class="device-name">'+esc(pst.name)+' <span class="'+(isAgent?'driver-badge':'driver-badge plain')+'">'+(isAgent?'AI agent · pad':'use-case')+'</span></span>';
        h+='<span class="device-path">'+esc(pst.description||'')+'</span>';
        if(legend)h+='<span class="device-legend">'+esc(legend)+'</span>';
        h+='</div><div class="device-check"><span class="device-check-icon">→</span></div></div>';
      }
      h+='</div>';
    }
    h+='<div class="wizard-actions"><button class="btn" onclick="currentView=\'profile\';refresh();">Back</button></div>';
    h+='</div>';
    document.getElementById('editor').innerHTML=h;
  }else if(presetState.step===1){
    const ag=presetState.agent;
    let h='<div class="wizard-page">';
    h+=wizardProgress(2,2);
    h+='<h2>Create '+esc(ag?ag.name:'')+' pad</h2>';
    h+='<div class="prep-list">';
    h+='<div class="prep-item"><strong>Profile name</strong><p>Shown in the profile list.</p><input id="psName" value="'+esc(ag?ag.name:'')+' pad" style="width:100%"></div>';
    h+='<div class="prep-item"><strong>Target window</strong><p>Now (recommended) or set later in the profile. Keys go to this app even when unfocused.</p><div style="display:flex;gap:8px;align-items:center;margin-top:6px"><button class="btn" onclick="presetPickWindow()">Pick foreground app</button><span id="psTarget" class="prep-status">— not picked (uses default)</span></div></div>';
    h+='</div>';
    h+='<div class="wizard-actions"><button class="btn" onclick="presetState.step=0;renderPresetWizard()">Back</button><button class="btn primary" onclick="presetApply()">Create profile</button></div>';
    h+='</div>';
    document.getElementById('editor').innerHTML=h;
  }
}
function presetAgent(i){
  const ag=windowPresetList[i];
  if(!ag)return;
  presetState.agent=ag;
  presetState.step=1;
  renderPresetWizard();
}
async function presetPickWindow(){
  try{
    const r=await api('GET','/api/v1/windows/foreground');
    if(r.found){
      presetState.target={windowClass:r.windowClass||'',processName:r.processName||'',exePath:r.processPath||''};
      const el=document.getElementById('psTarget');
      if(el){el.textContent='picked: '+(r.processName||r.windowClass||'window');el.className='prep-status ok';}
      toast('Target set: '+(r.processName||r.windowClass),'success');
    }else{ toast('No foreground window detected','error'); }
  }catch(e){ toast('Pick failed: '+e.message,'error'); }
}
async function presetApply(){
  if(!presetState.agent)return;
  const nameEl=document.getElementById('psName');
  const presetName=(nameEl&&nameEl.value.trim())||(presetState.agent.name+' pad');
  const body={agentId:presetState.agent.agentId,name:presetName};
  const t=presetState.target||{};
  if(t.windowClass)body.windowClass=t.windowClass;
  if(t.processName)body.processName=t.processName;
  if(t.exePath)body.exePath=t.exePath;
  try{
    const r=await api('POST','/api/v1/preset/apply',body);
    if(r.ok){
      toast('Agent pad profile created','success');
      await refresh();
      selectProfile(presetName);
      toast('Now mapping F-keys for that agent — edit in the profile.','success');
    }else{ toast('Create failed: '+(r.error||'unknown'),'error'); }
  }catch(e){ toast('Preset failed: '+e.message,'error'); }
}
// ---- Live screen: active profile + recent actions ----
let liveTimer=null;
function stopLiveTimer(){ if(liveTimer){clearInterval(liveTimer);liveTimer=null;} }
function showLive(){
  stopAllPolling();
  currentView='live';
  stopLiveTimer();
  renderLive();
}
async function renderLive(){
  document.getElementById('editor').innerHTML='<div class="toolbar-row" style="display:flex;justify-content:space-between;align-items:center;margin-bottom:12px"><h2>Live — active profile</h2><button class="btn" onclick="showLive()">Refresh</button></div><div class="live-grid" id="liveGrid"><div class="info-callout">Loading…</div></div><div class="card-heading"><div><h3>Recent actions</h3></div></div><div class="live-ticker" id="liveTicker"><span class="muted">No activity yet — press keys on the dedicated keyboard.</span></div>';
  try{
    const st=await api('GET','/api/v1/state');
    const profiles=await api('GET','/api/profiles');
    state=profiles;
    renderSidebar();
    const active=profiles.active||st.active||'';
    const prof=profiles.profiles.find(p=>p.name===active);
    const g=document.getElementById('liveGrid');
    if(g){
      let h='';
      const keys=(prof&&prof.keys)||[];
      if(!keys.length){
        h='<div class="info-callout"><strong>No mappings in the active profile.</strong><span>Add key mappings, or create an Agent pad preset.</span></div>';
      }else{
        h+='<div class="card-heading"><div><span class="eyebrow">Active: '+esc(active)+'</span></div></div>';
        for(const k of keys){
          h+='<div class="live-cell"><span class="key-badge">'+esc(keyName(k.usage))+'</span>'+(k.mod?'<span class="layer-badge">Fn</span>':'')+'<div class="live-cell-action"><strong>'+esc(describeMacro(k.action))+'</strong><code>'+esc(k.action)+'</code><button type="button" class="btn small fire-btn" data-usage="'+k.usage+'" data-action="'+esc(k.action)+'" data-profile="'+esc(active)+'" onclick="fireCell(this)">▶ Fire</button></div></div>';
        }
      }
      g.innerHTML=h;
    }
  }catch(e){}
  liveTimer=setInterval(pollLive,800);
}
// Live click-to-fire: POST /api/v1/action/fire — run an action as if a key were pressed
async function fireCell(btn){
  const usage=parseInt(btn.getAttribute('data-usage')||'0',10);
  const action=btn.getAttribute('data-action')||'';
  const profile=btn.getAttribute('data-profile')||'';
  if(!action){toast('No action to fire','error');return;}
  btn.disabled=true;
  try{
    const r=await api('POST','/api/v1/action/fire',{action:action,usage:usage,profile:profile});
    if(r.ok){toast('Fired: '+actionDescription(action),'success');}
    else{toast('Fired failed: '+(r.error||'unknown'),'error');}
  }catch(e){toast('Fire failed: '+e.message,'error');}
  btn.disabled=false;
}
async function pollLive(){
  if(currentView!=='live'){stopLiveTimer();return;}
  try{
    const r=await api('GET','/api/v1/activity');
    const ev=r.events||[];
    const t=document.getElementById('liveTicker');
    if(!t)return;
    if(!ev.length){ t.innerHTML='<span class="muted">No activity yet.</span>'; return; }
    const items=ev.slice(-10);
    const lastSeq=ev[ev.length-1]?ev[ev.length-1].seq:0;
    let h='';
    for(const e of items){
      h+='<div class="live-hit'+(e.seq===lastSeq?' last':'')+'"><span class="key-badge">'+esc(keyName(e.usage))+'</span><div class="live-cell-action"><strong>'+esc(describeMacro(e.action))+'</strong><code>'+esc(e.action)+'</code></div><span class="live-mode">'+esc(e.mode)+'</span><button type="button" class="btn small fire-btn" data-usage="'+e.usage+'" data-action="'+esc(e.action)+'" data-profile="" onclick="fireCell(this)">▶ Fire</button></div>';
    }
    t.innerHTML=h;
  }catch(e){}
}
// ---- Export / Import config (base64) + combo builder ----
function comboInsert(){
  const m=document.getElementById('comboMod');
  const k=document.getElementById('comboKey');
  if(!m||!k)return;
  const mv=m.value;
  const kv=k.value.trim();
  if(!kv){toast('Enter a key first (B / F5 / Enter)','error');return;}
  const base=kv.replace(/[{}]/g,'');
  chipInsert(mv?('{'+mv+'+'+base+'}'):('{'+base+'}'),true);
}
async function exportConfig(){
  try{
    const r=await api('GET','/api/v1/config/export');
    if(!r.config){toast('Export failed: no config','error');return;}
    const bin=atob(r.config);
    const bytes=new Uint8Array(bin.length);
    for(let i=0;i<bin.length;i++)bytes[i]=bin.charCodeAt(i);
    const blob=new Blob([bytes],{type:'text/plain'});
    const url=URL.createObjectURL(blob);
    const a=document.createElement('a');
    a.href=url;a.download='keysidekick-config.ini';
    document.body.appendChild(a);a.click();a.remove();
    URL.revokeObjectURL(url);
    // One-click bonus: also copy the INI to the clipboard (two actions, one click)
    try{
      const text=new TextDecoder().decode(bytes);
      await navigator.clipboard.writeText(text);
      toast('Config exported — file downloaded and INI copied to clipboard','success');
    }catch(e){toast('Config exported','success');}
  }catch(e){toast('Export failed: '+e.message,'error');}
}
function showOnboarding(){localStorage.removeItem('ks_onboarded');currentView='profile';renderSidebar();renderOnboarding();}
function importConfigFile(file){
  const reader=new FileReader();
  reader.onload=async()=>{
    try{
      const bytes=new Uint8Array(reader.result);
      let bin='';
      for(let i=0;i<bytes.length;i++)bin+=String.fromCharCode(bytes[i]);
      const b64=btoa(bin);
      const r=await api('POST','/api/v1/config/import',{config:b64});
      if(r.ok){
        toast('Config imported & reloaded','success');
        await refresh();
      }else{
        toast('Import failed: '+(r.error||'invalid config'),'error');
      }
    }catch(e){toast('Import failed: '+e.message,'error');}
  };
  reader.readAsArrayBuffer(file);
}
// Phase 4: SSE live updates — replaces 3s polling with event-driven refresh
refresh();
// Version footer (from /api/v1/state)
async function loadVersion(){try{const s=await api('GET','/api/v1/state');const f=document.getElementById('versionFooter');if(f&&s.version)f.textContent='KeySidekick '+s.version;}catch(e){}}
loadVersion();
let sse=null;let pollFallback=null;
function connectSSE(){
  if(sse){try{sse.close();}catch(e){}}
  try{
    sse=new EventSource('/api/v1/events');
    sse.addEventListener('revision',async(e)=>{
      try{
        const d=JSON.parse(e.data);
        if(d.revision&&d.revision!==LAST_REVISION){
          LAST_REVISION=d.revision;
          if(profileDirty){renderSidebar();return;}
          await refresh();
        }
      }catch(err){}
    });
    sse.addEventListener('activity',()=>{
      if(currentView==='live')pollLive();
    });
    sse.onerror=()=>{
      // Fallback to polling if SSE fails
      if(!pollFallback){pollFallback=setInterval(refresh,3000);}
      try{sse.close();}catch(e){}sse=null;
      setTimeout(connectSSE,5000); // reconnect after 5s
    };
    // If SSE connected, stop polling fallback
    sse.onopen=()=>{if(pollFallback){clearInterval(pollFallback);pollFallback=null;}};
  }catch(e){
    if(!pollFallback){pollFallback=setInterval(refresh,3000);}
  }
}
connectSSE();
