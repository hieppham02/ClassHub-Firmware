$ErrorActionPreference = 'Stop'
$path = 'E:\EAUT\Khoi_nghiep_sang_tao\ClassHub\Frontend\src\views\client\HomeView.vue'
$source = [IO.File]::ReadAllText($path)
$anchor = 'const selectedCabinet = ref(null)'
$computed = @'
const selectedCabinet = ref(null)
const cabinetEquipment = computed(() => {
  const cabinet = selectedCabinet.value
  if (!cabinet) return []
  const catalog = (cabinet.equipment || []).map(item => typeof item === 'object' ? item : { name: item, image: '' })
  const details = cabinet.equipmentDetails || []
  return [
    ...details.map(item => ({ ...catalog.find(entry => entry.name === item.name), ...item })),
    ...catalog.filter(item => !details.some(detail => detail.name === item.name))
  ]
})
'@
$start = $source.IndexOf('          <ul v-if="selectedCabinet.equipmentDetails?.length"')
$end = $source.IndexOf('          <template v-if="activeBooking', $start)
if ($start -lt 0 -or $end -lt 0 -or -not $source.Contains($anchor)) { throw 'Layout anchors missing' }
$layout = @'
          <p class="mb-3 text-sm font-bold uppercase tracking-wide text-slate-400">{{ selectedCabinet.locationType === 'CORRIDOR' ? 'Đồ dùng chung · Hành lang' : 'Thiết bị trong tủ' }}</p>
          <p v-if="selectedCabinet.locationType === 'CORRIDOR'" class="mb-4 text-sm text-slate-500">Quy trình mượn và trả giống tủ phòng học.</p>
          <div v-if="cabinetEquipment.length" class="mb-6 space-y-2">
            <button v-for="(item, index) in cabinetEquipment" :key="item.id ?? `${item.name}-${index}`"
              type="button" @click="previewEquipment = item"
              class="flex w-full items-center gap-3 rounded-xl border border-slate-200 bg-slate-50 p-3 text-left transition hover:border-blue-400 hover:bg-blue-50 focus-visible:outline-2 focus-visible:outline-blue-500">
              <span class="flex size-12 shrink-0 items-center justify-center overflow-hidden rounded-lg border border-slate-200 bg-white text-lg font-bold text-blue-600">
                <img v-if="item.image" :src="item.image" alt="" class="size-full object-contain" />
                <span v-else>{{ item.name?.charAt(0) }}</span>
              </span>
              <span class="min-w-0 flex-1">
                <span class="block text-sm font-semibold text-slate-800">{{ item.name }}</span>
                <span v-if="item.quantity != null" class="mt-1 block text-xs text-slate-500">Số lượng: {{ item.quantity }}</span>
                <span v-if="item.note" class="mt-1 block text-xs text-slate-500">{{ item.note }}</span>
              </span>
              <span v-if="item.condition" class="shrink-0 rounded-full px-2 py-1 text-xs font-semibold"
                :class="item.condition === 'TOT' ? 'bg-emerald-100 text-emerald-700' : item.condition === 'HONG' ? 'bg-red-100 text-red-700' : 'bg-amber-100 text-amber-700'">
                {{ { TOT: 'Tốt', HONG: 'Hỏng', DANG_BAO_TRI: 'Đang bảo trì' }[item.condition] || item.condition }}
              </span>
            </button>
          </div>
          <p v-else class="mb-6 text-sm text-slate-500">Chưa có thông tin thiết bị trong tủ.</p>

'@
$source = $source.Substring(0, $start) + $layout + $source.Substring($end)
$source = $source.Replace($anchor, $computed)
[IO.File]::WriteAllText($path, $source, [Text.UTF8Encoding]::new($false))
