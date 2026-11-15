'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const html = fs.readFileSync('phone-callback/index.html', 'utf8');
const source = html.match(/<script>([\s\S]*?)<\/script>/)[1];
function run(search, address = '192.168.1.42') {
  let submitted, target, cleaned;
  const nodes = {message:{textContent:''}, transfer:{hidden:true, addEventListener(_event, fn){submitted=fn;}}, address:{value:address}};
  const location = {search, pathname:'/callback/', replace(value){target=value;}};
  vm.runInNewContext(source, {document:{getElementById(id){return nodes[id];}}, location, history:{replaceState(_a,_b,value){cleaned=value;}}, URLSearchParams});
  if (submitted) submitted({preventDefault(){}});
  return {nodes, target, cleaned};
}
const state = 'A'.repeat(22);
let result = run('?code=one%2Btwo&state=' + state);
assert.strictEqual(result.cleaned, '/callback/');
assert.strictEqual(result.target, 'http://192.168.1.42:38741/callback?code=one%2Btwo&state=' + state);
for (const address of ['8.8.8.8', '127.0.0.1', '192.168.1.255', '192.168.1.999', 'evil.com', '192.168.01.1', '192.168.1.1@evil.com']) assert.strictEqual(run('?code=abc&state=' + state, address).target, undefined);
for (const query of ['?code=a&code=b&state=' + state, '?code=a', '?error=access_denied', '?code=a&state=wrong']) assert.strictEqual(run(query).target, undefined);
console.log('Phone callback: valid, cancelled, malformed and untrusted-address cases passed.');
