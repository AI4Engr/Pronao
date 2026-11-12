document.getElementById('origin').textContent = location.origin;
const out = document.getElementById('out');
const show = (label, p) => p.then(v => { out.textContent = label + ' -> ' + JSON.stringify(v); },
                                  e => { out.textContent = label + ' rejected: ' + JSON.stringify(e); });
document.getElementById('greet').onclick = () => show('greet', pronao.invoke('greet', { name: document.getElementById('name').value }));
document.getElementById('sum').onclick = () => show('sum', pronao.invoke('sum', [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]));
// an address the folder does not have is a 404 from the program, not a request to the network
document.getElementById('missing').onclick = () => show('GET /missing', fetch('/missing').then(r => r.status));
