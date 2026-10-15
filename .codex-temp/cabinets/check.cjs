const fs = require('node:fs')
const assert = require('node:assert/strict')
const { parse, compileScript, compileTemplate } = require('E:/EAUT/Khoi_nghiep_sang_tao/ClassHub/Frontend/node_modules/@vue/compiler-sfc')
const filename = 'E:/EAUT/Khoi_nghiep_sang_tao/ClassHub/Frontend/src/views/client/HomeView.vue'
const source = fs.readFileSync(filename, 'utf8')
const { descriptor, errors } = parse(source, { filename })
if (errors.length) throw errors[0]
compileScript(descriptor, { id: 'cabinet-check' })
const result = compileTemplate({ source: descriptor.template.content, filename, id: 'cabinet-check' })
if (result.errors.length) throw result.errors[0]
const match = source.match(/return Object\.fromEntries\(Object\.entries\(groups\)[\s\S]*?\]\)\)/)
assert.ok(match)
const groups = { DTD: { 'Tầng 10': [], 'Tầng 3': [], 'Tầng 1': [], 'Tầng 2': [] } }
const sorted = new Function('groups', match[0])(groups)
assert.deepEqual(Object.keys(sorted.DTD), ['Tầng 1', 'Tầng 2', 'Tầng 3', 'Tầng 10'])
assert.ok(!source.includes('flex-col-reverse gap-8'))
assert.ok(source.includes('h-32 sm:h-36'))
console.log('Vue compilation and ascending floor order passed')
